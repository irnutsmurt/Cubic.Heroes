// Native BulletCD (bullet_cd.h). The flow follows Bullet's
// SpuGatheringCollisionTask (processCollisionTask, handleCollisionPair,
// ProcessConvexConcaveSpuCollision, ProcessSpuConvexConvexCollision) and
// SpuContactResult, and the manifold cache follows btPersistentManifold; the
// GJK, its simplex solver, the Minkowski penetration solver and the box-box
// detector are Bullet 2.74's own, compiled for the host. Bullet: Copyright
// (c) 2003-2006 Erwin Coumans, zlib licence (fetched by tools/build_deps.py).
//
// The game's data stays where it is, in guest memory, read and written
// big-endian at the layouts docs/BULLET_NATIVE.md records (Bullet 2.73,
// 32-bit pointers). A manifold point is kept as its raw 0xB0 bytes, so the
// bytes Bullet never sets -- a new point's part ids, its padding, its
// friction directions -- move exactly as the task moves them.
#include "bullet_cd.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "LinearMath/btTransform.h"
#include "BulletCollision/BroadphaseCollision/btBroadphaseProxy.h"
#include "BulletCollision/CollisionShapes/btBoxShape.h"
#include "BulletCollision/CollisionDispatch/btBoxBoxDetector.h"
#include "BulletCollision/NarrowPhaseCollision/btDiscreteCollisionDetectorInterface.h"
#include "BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuContactResult.h"
#include "BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuGjkPairDetector.h"
#include "BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuVoronoiSimplexSolver.h"
#include "BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuMinkowskiPenetrationDepthSolver.h"
#include "BulletMultiThreaded/SpuNarrowPhaseCollisionTask/SpuCollisionShapes.h"

namespace bullet_cd {
namespace {

// ---- guest memory -----------------------------------------------------------
inline uint32_t r32(const Mem& m, uint32_t ea)
{
    const uint8_t* p = m.base + ea;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
inline uint16_t r16(const Mem& m, uint32_t ea) { const uint8_t* p = m.base + ea; return (uint16_t)(p[0] << 8 | p[1]); }
inline float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
inline uint32_t u32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float rf(const Mem& m, uint32_t ea) { return f32(r32(m, ea)); }
inline btVector3 rv3(const Mem& m, uint32_t ea) { return btVector3(rf(m, ea), rf(m, ea + 4), rf(m, ea + 8)); }
inline btTransform rtr(const Mem& m, uint32_t ea)   // basis rows at +0, +0x10, +0x20; origin at +0x30
{
    return btTransform(btMatrix3x3(rf(m, ea), rf(m, ea + 4), rf(m, ea + 8),
                                   rf(m, ea + 0x10), rf(m, ea + 0x14), rf(m, ea + 0x18),
                                   rf(m, ea + 0x20), rf(m, ea + 0x24), rf(m, ea + 0x28)),
                       rv3(m, ea + 0x30));
}

// Bullet 2.73's shape types (no 2D types) and activation states
enum { kBox = 0, kTriangle = 1, kCylinder = 13, kTriangleMesh = 19 };
enum { kIslandSleeping = 2, kDisableSimulation = 5 };
// SpuGatherAndProcessWorkUnitInput pages: 16 bytes, one unit each
enum { kWorkUnitPage = 16 };

// Layouts (docs/BULLET_NATIVE.md)
namespace L {
enum : uint32_t {
    DescInPtr = 0x00, DescNumOnLastPage = 0x0C, DescNumPages = 0x10,
    AlgDim0 = 0x10, AlgDim1 = 0x20, AlgManifold = 0x30, AlgType0 = 0x34, AlgType1 = 0x38,
    AlgMargin0 = 0x3C, AlgMargin1 = 0x40, AlgObj0 = 0x44, AlgObj1 = 0x48,
    ObjTransform = 0x10, ObjShape = 0xB4, ObjActivation = 0xC8, ObjFriction = 0xD0, ObjRestitution = 0xD4,
    ShapeType = 0x04, ShapeImplicitDims = 0x20, ShapeMargin = 0x30,
    CylUpAxis = 0x68,   // btCylinderShape::m_upAxis (a live Y-up cylinder: 0 at +0x64, 1 here)
    MeshInterface = 0x30, MeshBvh = 0x34,
    IfaceScaling = 0x10, IfaceMeshCount = 0x24, IfaceMeshes = 0x2C,
    ImTriangles = 0x00, ImIndexBase = 0x04, ImIndexStride = 0x08, ImVertexBase = 0x10,
    ImVertexStride = 0x14, ImIndexType = 0x18,
    BvhMin = 0x10, BvhMax = 0x20, BvhQuant = 0x30, BvhVersion = 0x40, BvhUseQuant = 0x48,
    BvhNodeCount = 0x8C, BvhNodes = 0x94, BvhTraversal = 0x9C, BvhSubtreeCount = 0xA4, BvhSubtrees = 0xAC,
    MfBody0 = 0x2C0, MfCount = 0x2C8, MfBreaking = 0x2CC,
};
}
enum { kPhyShort = 3, kPhyInteger = 2 };

// ---- the manifold, as raw guest bytes --------------------------------------
// btManifoldPoint (0xB0): localA 0, localB 0x10, worldOnB 0x20, worldOnA 0x30,
// normalOnB 0x40, distance 0x50, friction 0x54, restitution 0x58, part/index
// ids 0x5C..0x68, user data 0x6C, applied impulse 0x70, lateral friction
// initialised 0x74 (u8), lateral impulses 0x78/0x7C, lifetime 0x80,
// padding 0x84..0x8C, friction directions 0x90/0xA0.
struct Point {
    uint8_t raw[0xB0];
    uint32_t g(uint32_t o) const { return (uint32_t)raw[o] << 24 | (uint32_t)raw[o + 1] << 16 | (uint32_t)raw[o + 2] << 8 | raw[o + 3]; }
    void s(uint32_t o, uint32_t v) { raw[o] = (uint8_t)(v >> 24); raw[o + 1] = (uint8_t)(v >> 16); raw[o + 2] = (uint8_t)(v >> 8); raw[o + 3] = (uint8_t)v; }
    btVector3 v(uint32_t o) const { return btVector3(f32(g(o)), f32(g(o + 4)), f32(g(o + 8))); }
    void setv(uint32_t o, const btVector3& x) { s(o, u32(x.getX())); s(o + 4, u32(x.getY())); s(o + 8, u32(x.getZ())); s(o + 12, 0); }
    float f(uint32_t o) const { return f32(g(o)); }
    void setf(uint32_t o, float x) { s(o, u32(x)); }
};
enum : uint32_t { PLocalA = 0x00, PLocalB = 0x10, PWorldB = 0x20, PWorldA = 0x30, PNormal = 0x40, PDist = 0x50,
                  PFriction = 0x54, PRestitution = 0x58, PUser = 0x6C, PImpulse = 0x70, PLatInit = 0x74,
                  PLat1 = 0x78, PLat2 = 0x7C, PLife = 0x80 };

struct Manifold {
    uint32_t ea;
    uint8_t raw[kManifoldBytes];
    Point pt[4];
    int count;
    float breaking;

    void load(const Mem& m, uint32_t at)
    {
        ea = at;
        memcpy(raw, m.base + at, kManifoldBytes);
        for (int i = 0; i < 4; i++) memcpy(pt[i].raw, raw + i * 0xB0, 0xB0);
        count = (int)r32(m, at + L::MfCount);
        breaking = rf(m, at + L::MfBreaking);
    }
    void store(uint8_t* out) const
    {
        memcpy(out, raw, kManifoldBytes);
        for (int i = 0; i < 4; i++) memcpy(out + i * 0xB0, pt[i].raw, 0xB0);
        const uint32_t c = (uint32_t)count;
        out[L::MfCount] = (uint8_t)(c >> 24); out[L::MfCount + 1] = (uint8_t)(c >> 16);
        out[L::MfCount + 2] = (uint8_t)(c >> 8); out[L::MfCount + 3] = (uint8_t)c;
    }

    // btPersistentManifold::getCacheEntry
    int cacheEntry(const btVector3& localA) const
    {
        btScalar shortest = breaking * breaking;
        int nearest = -1;
        for (int i = 0; i < count; i++) {
            const btVector3 d = pt[i].v(PLocalA) - localA;
            const btScalar d2 = d.dot(d);
            if (d2 < shortest) { shortest = d2; nearest = i; }
        }
        return nearest;
    }
    // btPersistentManifold::sortCachedPoints (KEEP_DEEPEST_POINT)
    int sortCached(const btVector3& localA, float dist) const
    {
        int deepest = -1;
        btScalar maxPen = dist;
        for (int i = 0; i < 4; i++)
            if (pt[i].f(PDist) < maxPen) { deepest = i; maxPen = pt[i].f(PDist); }
        btScalar res[4] = { 0, 0, 0, 0 };
        const btVector3 a[4] = { pt[0].v(PLocalA), pt[1].v(PLocalA), pt[2].v(PLocalA), pt[3].v(PLocalA) };
        if (deepest != 0) res[0] = (localA - a[1]).cross(a[3] - a[2]).length2();
        if (deepest != 1) res[1] = (localA - a[0]).cross(a[3] - a[2]).length2();
        if (deepest != 2) res[2] = (localA - a[0]).cross(a[3] - a[1]).length2();
        if (deepest != 3) res[3] = (localA - a[0]).cross(a[2] - a[1]).length2();
        return btVector4(res[0], res[1], res[2], res[3]).closestAxis4();
    }
    // btPersistentManifold::addManifoldPoint (no contact-destroyed callback on the SPU)
    void add(const Point& p)
    {
        int at = count;
        if (at == 4) at = sortCached(p.v(PLocalA), p.f(PDist));
        else count++;
        if (at < 0) at = 0;
        pt[at] = p;
    }
    // btPersistentManifold::removeContactPoint
    void remove(int i)
    {
        const int last = count - 1;
        if (i != last) {
            pt[i] = pt[last];
            pt[last].s(PUser, 0);
            pt[last].setf(PImpulse, 0.f);
            pt[last].raw[PLatInit] = 0;
            pt[last].setf(PLat1, 0.f);
            pt[last].setf(PLat2, 0.f);
            pt[last].s(PLife, 0);
        }
        count--;
    }
    // btPersistentManifold::refreshContactPoints (no contact-processed callback)
    void refresh(const btTransform& trA, const btTransform& trB)
    {
        for (int i = count - 1; i >= 0; i--) {
            Point& p = pt[i];
            const btVector3 wa = trA(p.v(PLocalA)), wb = trB(p.v(PLocalB));
            p.setv(PWorldA, wa);
            p.setv(PWorldB, wb);
            p.setf(PDist, (wa - wb).dot(p.v(PNormal)));
            p.s(PLife, p.g(PLife) + 1);
        }
        for (int i = count - 1; i >= 0; i--) {
            const Point& p = pt[i];
            if (!(p.f(PDist) <= breaking)) { remove(i); continue; }
            const btVector3 projected = p.v(PWorldA) - p.v(PNormal) * p.f(PDist);
            const btVector3 diff = p.v(PWorldB) - projected;
            if (diff.dot(diff) > breaking * breaking) remove(i);
        }
    }
};

struct Ctx {
    const Mem* m;
    Result* out;
    bool write;
    uint32_t types;   // the pair being done: type0 << 8 | type1
};

}  // namespace
}  // namespace bullet_cd

// ---- SpuContactResult, over a bullet_cd::Manifold ---------------------------
// Bullet's declaration (SpuContactResult.h), these definitions: the manifold
// pointer it holds is a bullet_cd::Manifold, and flush hands the result to the
// task instead of a DMA put.
namespace { thread_local bullet_cd::Ctx* t_ctx; }

SpuContactResult::SpuContactResult() : m_manifoldAddress(0), m_spuManifold(0), m_RequiresWriteBack(false) {}
SpuContactResult::~SpuContactResult() {}
void SpuContactResult::setShapeIdentifiers(int, int, int, int) {}   // the SPU task ignores them too
void SpuContactResult::writeDoubleBufferedManifold(btPersistentManifold*, btPersistentManifold*) {}

static inline btScalar combined_friction(btScalar f0, btScalar f1)
{
    btScalar f = f0 * f1;
    if (f < btScalar(-10.)) f = btScalar(-10.);
    if (f > btScalar(10.)) f = btScalar(10.);
    return f;
}

void SpuContactResult::setContactInfo(btPersistentManifold* spuManifold, ppu_address_t manifoldAddress,
                                      const btTransform& worldTrans0, const btTransform& worldTrans1,
                                      btScalar restitution0, btScalar restitution1,
                                      btScalar friction0, btScalar friction1, bool isSwapped)
{
    m_rootWorldTransform0 = worldTrans0;
    m_rootWorldTransform1 = worldTrans1;
    m_manifoldAddress = manifoldAddress;
    m_spuManifold = spuManifold;
    m_combinedFriction = combined_friction(friction0, friction1);
    m_combinedRestitution = restitution0 * restitution1;
    m_isSwapped = isSwapped;
}

// ManifoldResultAddContactPoint: a point near a cached one changes nothing.
void SpuContactResult::addContactPoint(const btVector3& normalOnBInWorld, const btVector3& pointInWorld, float depth)
{
    bullet_cd::Manifold* mf = reinterpret_cast<bullet_cd::Manifold*>(m_spuManifold);
    if (t_ctx) t_ctx->out->contacts++;
    if (depth > mf->breaking) return;
    const btTransform invA = m_rootWorldTransform0.inverse(), invB = m_rootWorldTransform1.inverse();
    const btVector3 normal = m_isSwapped ? normalOnBInWorld * -1 : normalOnBInWorld;
    const btVector3 pointA = pointInWorld + normal * depth;
    const btVector3 localA = invA(pointA), localB = invB(pointInWorld);
    if (mf->cacheEntry(localA) >= 0) return;
    // btManifoldPoint(localA, localB, normal, depth); the fields its
    // constructor leaves alone are zero here, whatever was on the SPU's stack.
    bullet_cd::Point p;
    memset(p.raw, 0, sizeof p.raw);
    p.setv(bullet_cd::PLocalA, localA);
    p.setv(bullet_cd::PLocalB, localB);
    p.setv(bullet_cd::PNormal, normal);
    p.setf(bullet_cd::PDist, depth);
    p.setf(bullet_cd::PFriction, m_combinedFriction);
    p.setf(bullet_cd::PRestitution, m_combinedRestitution);
    mf->add(p);
    m_RequiresWriteBack = true;
}

void SpuContactResult::flush()
{
    bullet_cd::Manifold* mf = reinterpret_cast<bullet_cd::Manifold*>(m_spuManifold);
    if (mf && mf->count) {
        mf->refresh(m_rootWorldTransform0, m_rootWorldTransform1);
        m_RequiresWriteBack = true;
    }
    if (mf && m_RequiresWriteBack && t_ctx) {
        bullet_cd::ManifoldOut o;
        o.ea = mf->ea;
        o.types = t_ctx->types;
        mf->store(o.bytes);
        if (t_ctx->write) memcpy(t_ctx->m->base + mf->ea, o.bytes, bullet_cd::kManifoldBytes);
        t_ctx->out->manifolds.push_back(o);
    }
    m_spuManifold = 0;
    m_RequiresWriteBack = false;
}

// A cylinder as the support function reads it: btCylinderShape's implicit
// half extents and up axis.
struct GuestCylinder {
    btVector3 dims;
    int up;
};

// The SPU task's support function, for the shapes handled here.
btVector3 localGetSupportingVertexWithoutMargin(int shapeType, void* shape, const btVector3& localDir,
                                                struct SpuConvexPolyhedronVertexData*)
{
    if (shapeType == BOX_SHAPE_PROXYTYPE) {
        const btVector3& h = ((btConvexInternalShape*)shape)->getImplicitShapeDimensions();
        return btVector3(localDir.getX() < 0.0f ? -h.x() : h.x(),
                         localDir.getY() < 0.0f ? -h.y() : h.y(),
                         localDir.getZ() < 0.0f ? -h.z() : h.z());
    }
    if (shapeType == CYLINDER_SHAPE_PROXYTYPE) {
        // radius and half height by up axis, as SpuCollisionShapes.cpp maps them
        const GuestCylinder& c = *(const GuestCylinder*)shape;
        const int XX = c.up == 1 ? 0 : c.up == 2 ? 0 : 1, YY = c.up, ZZ = c.up == 2 ? 1 : 2;
        const btScalar radius = c.dims[XX], half_height = c.dims[c.up];
        btVector3 tmp;
        const btScalar s = btSqrt(localDir[XX] * localDir[XX] + localDir[ZZ] * localDir[ZZ]);
        if (s != btScalar(0.0)) {
            const btScalar d = radius / s;
            tmp[XX] = localDir[XX] * d;
            tmp[YY] = localDir[YY] < 0.0 ? -half_height : half_height;
            tmp[ZZ] = localDir[ZZ] * d;
        } else {
            tmp[XX] = radius;
            tmp[YY] = localDir[YY] < 0.0 ? -half_height : half_height;
            tmp[ZZ] = btScalar(0.0);
        }
        return tmp;
    }
    // TRIANGLE_SHAPE_PROXYTYPE: the vertex furthest along the direction
    const btVector3* v = (const btVector3*)shape;
    const btVector3 dots(localDir.dot(v[0]), localDir.dot(v[1]), localDir.dot(v[2]));
    return v[dots.maxAxis()];
}

namespace bullet_cd {
namespace {

// A box shape with the guest's own implicit half extents and margin.
struct GuestBox : btBoxShape {
    GuestBox(const btVector3& implicit, btScalar margin) : btBoxShape(implicit + btVector3(margin, margin, margin))
    {
        m_implicitShapeDimensions = implicit;
        m_collisionMargin = margin;
    }
};

// The guest's box or cylinder shape, as the support function reads it
struct GuestConvex {
    int type;
    btVector3 implicit;
    btScalar margin;
    GuestBox box;
    GuestCylinder cyl;
    GuestConvex(const Mem& m, uint32_t shape, int t)
        : type(t), implicit(rv3(m, shape + L::ShapeImplicitDims)), margin(rf(m, shape + L::ShapeMargin)),
          box(implicit, margin), cyl{ implicit, t == kCylinder ? (int)r32(m, shape + L::CylUpAxis) : 1 } {}
    void* ptr() const { return type == kCylinder ? (void*)&cyl : (void*)&box; }
};

struct BridgeResult : btDiscreteCollisionDetectorInterface::Result {
    SpuContactResult& c;
    explicit BridgeResult(SpuContactResult& r) : c(r) {}
    void setShapeIdentifiers(int a, int b, int x, int y) override { c.setShapeIdentifiers(a, b, x, y); }
    void addContactPoint(const btVector3& n, const btVector3& p, btScalar d) override { c.addContactPoint(n, p, d); }
};

bool is_active(const Mem& m, uint32_t obj)
{
    const uint32_t s = r32(m, obj + L::ObjActivation);
    return s != kIslandSleeping && s != kDisableSimulation;
}

// One pair's inputs as handleCollisionPair sees them, after the swap.
struct PairIn {
    uint32_t obj0, obj1;            // in the pair's own order: contact info and refresh use them
    btTransform root0, root1;       // ... their world transforms
    float rest0, rest1, fric0, fric1;
    btTransform world0, world1;     // swapped: the convex shape's first
    int type0, type1;
    float margin0, margin1;
    uint32_t shape0, shape1;        // guest shapes, swapped
    bool swapped;
};

// ProcessSpuConvexConvexCollision: GJK with the Minkowski penetration solver
void convex_convex(const PairIn& in, const Manifold& mf, SpuContactResult& contacts,
                   void* shape0, int type0, void* shape1, int type1)
{
    SpuVoronoiSimplexSolver simplex;
    SpuMinkowskiPenetrationDepthSolver penetration;
    SpuClosestPointInput cp;
    cp.m_convexVertexData[0] = cp.m_convexVertexData[1] = 0;
    cp.m_transformA = in.world0;
    cp.m_transformB = in.world1;
    const float sum = in.margin0 + in.margin1 + mf.breaking;
    cp.m_maximumDistanceSquared = sum * sum;
    SpuGjkPairDetector gjk(shape0, shape1, type0, type1, in.margin0, in.margin1, &simplex, &penetration);
    gjk.getClosestPoints(cp, contacts);
}

// spuNodeCallback::processNode + ProcessSpuConvexConvexCollision against one triangle
void triangle(const Mem& m, const PairIn& in, Manifold& mf, SpuContactResult& contacts,
              const GuestConvex& convex, uint32_t mesh_im, const btVector3& scaling, int triangle_index)
{
    const uint32_t ibase = r32(m, mesh_im + L::ImIndexBase), istride = r32(m, mesh_im + L::ImIndexStride);
    const uint32_t vbase = r32(m, mesh_im + L::ImVertexBase), vstride = r32(m, mesh_im + L::ImVertexStride);
    const uint32_t at = ibase + (uint32_t)triangle_index * istride;
    uint32_t idx[3];
    if (r32(m, mesh_im + L::ImIndexType) == kPhyShort)
        for (int k = 0; k < 3; k++) idx[k] = r16(m, at + 2u * k);
    else
        for (int k = 0; k < 3; k++) idx[k] = r32(m, at + 4u * k);
    ATTRIBUTE_ALIGNED16(btVector3 verts[3]);
    for (int j = 2; j >= 0; j--) {
        const uint32_t v = vbase + idx[j] * vstride;
        verts[j] = btVector3(rf(m, v) * scaling.getX(), rf(m, v + 4) * scaling.getY(), rf(m, v + 8) * scaling.getZ());
    }
    t_ctx->out->triangles++;
    contacts.setContactInfo(reinterpret_cast<btPersistentManifold*>(&mf), mf.ea, in.root0, in.root1,
                            in.rest0, in.rest1, in.fric0, in.fric1, in.swapped);
    convex_convex(in, mf, contacts, convex.ptr(), convex.type, (void*)verts, TRIANGLE_SHAPE_PROXYTYPE);
}

// ProcessConvexConcaveSpuCollision: the box or cylinder against the mesh's triangles
void convex_concave(const Mem& m, const PairIn& in, Manifold& mf, SpuContactResult& contacts)
{
    const uint32_t mesh = in.shape1, bvh = r32(m, mesh + L::MeshBvh), iface = r32(m, mesh + L::MeshInterface);
    const GuestConvex convex(m, in.shape0, in.type0);

    // computeAabb (box and cylinder alike) in the mesh's space
    const btTransform convex_in_mesh = in.world1.inverse() * in.world0;
    const btVector3 half = convex.implicit + btVector3(convex.margin, convex.margin, convex.margin);
    const btMatrix3x3 abs_b = convex_in_mesh.getBasis().absolute();
    const btVector3 center = convex_in_mesh.getOrigin();
    const btVector3 extent(abs_b[0].dot(half), abs_b[1].dot(half), abs_b[2].dot(half));
    const btVector3 lo = center - extent, hi = center + extent;

    // btQuantizedBvh::quantizeWithClamp
    const btVector3 bmin = rv3(m, bvh + L::BvhMin), bmax = rv3(m, bvh + L::BvhMax), quant = rv3(m, bvh + L::BvhQuant);
    unsigned short qmin[3], qmax[3];
    for (int is_max = 0; is_max < 2; is_max++) {
        btVector3 p = is_max ? hi : lo;
        p.setMax(bmin);
        p.setMin(bmax);
        const btVector3 v = (p - bmin) * quant;
        unsigned short* q = is_max ? qmax : qmin;
        for (int k = 0; k < 3; k++)
            q[k] = is_max ? (unsigned short)((unsigned short)(v[k] + btScalar(1.)) | 1)
                          : (unsigned short)((unsigned short)(v[k]) & 0xfffe);
    }

    const uint32_t subtrees = r32(m, bvh + L::BvhSubtreeCount), headers = r32(m, bvh + L::BvhSubtrees);
    const uint32_t nodes = r32(m, bvh + L::BvhNodes);
    if (!subtrees || r32(m, iface + L::IfaceMeshCount) != 1) return;   // the SPU task does nothing either
    const uint32_t im = r32(m, iface + L::IfaceMeshes);
    const btVector3 scaling = rv3(m, iface + L::IfaceScaling);
    auto overlap = [&](uint32_t at) {   // spuTestQuantizedAabbAgainstQuantizedAabb against qmin/qmax
        for (int k = 0; k < 3; k++)
            if (qmin[k] > r16(m, at + 6 + 2u * k) || qmax[k] < r16(m, at + 2u * k)) return false;
        return true;
    };
    for (uint32_t s = 0; s < subtrees; s++) {
        const uint32_t h = headers + s * 32u;
        if (!overlap(h)) continue;
        // spuWalkStacklessQuantizedTree over this subtree's nodes
        const int root = (int)r32(m, h + 12), size = (int)r32(m, h + 16);
        for (int cur = 0; cur < size;) {
            const uint32_t n = nodes + (uint32_t)(root + cur) * 16u;
            const int esc_or_tri = (int)r32(m, n + 12);
            const bool hit = overlap(n), leaf = esc_or_tri >= 0;
            if (leaf && hit) triangle(m, in, mf, contacts, convex, im, scaling, esc_or_tri & ~((~0) << (31 - 10)));
            cur += (hit || leaf) ? 1 : -esc_or_tri;
        }
    }
}

void pair(const Mem& m, uint32_t pair_ea)
{
    const uint32_t p0 = r32(m, pair_ea), p1 = r32(m, pair_ea + 4), alg = r32(m, pair_ea + 8);
    if (r32(m, pair_ea + 12) != 2 || !alg || !p0 || !p1) return;
    t_ctx->out->pairs++;
    t_ctx->types = r32(m, alg + L::AlgType0) << 8 | r32(m, alg + L::AlgType1);
    PairIn in;
    in.obj0 = r32(m, alg + L::AlgObj0);
    in.obj1 = r32(m, alg + L::AlgObj1);
    if (!is_active(m, in.obj0) && !is_active(m, in.obj1)) return;
    t_ctx->out->active++;
    Manifold mf;
    mf.load(m, r32(m, alg + L::AlgManifold));
    in.root0 = in.world0 = rtr(m, in.obj0 + L::ObjTransform);
    in.root1 = in.world1 = rtr(m, in.obj1 + L::ObjTransform);
    in.rest0 = rf(m, in.obj0 + L::ObjRestitution);
    in.rest1 = rf(m, in.obj1 + L::ObjRestitution);
    in.fric0 = rf(m, in.obj0 + L::ObjFriction);
    in.fric1 = rf(m, in.obj1 + L::ObjFriction);
    in.type0 = (int)r32(m, alg + L::AlgType0);
    in.type1 = (int)r32(m, alg + L::AlgType1);
    in.margin0 = rf(m, alg + L::AlgMargin0);
    in.margin1 = rf(m, alg + L::AlgMargin1);
    in.shape0 = r32(m, in.obj0 + L::ObjShape);
    in.shape1 = r32(m, in.obj1 + L::ObjShape);
    in.swapped = false;
    SpuContactResult contacts;

    if (in.type0 == kBox && in.type1 == kBox) {
        // btBoxBoxDetector on boxes of the algorithm's dimensions plus margin
        contacts.setContactInfo(reinterpret_cast<btPersistentManifold*>(&mf), mf.ea, in.root0, in.root1,
                                in.rest0, in.rest1, in.fric0, in.fric1, false);
        const btVector3 d0 = rv3(m, alg + L::AlgDim0) + btVector3(in.margin0, in.margin0, in.margin0);
        const btVector3 d1 = rv3(m, alg + L::AlgDim1) + btVector3(in.margin1, in.margin1, in.margin1);
        btBoxShape box0(d0), box1(d1);
        BridgeResult bridge(contacts);
        btDiscreteCollisionDetectorInterface::ClosestPointInput input;
        input.m_maximumDistanceSquared = 1e30f;
        input.m_transformA = in.world0;
        input.m_transformB = in.world1;
        btBoxBoxDetector detector(&box0, &box1);
        detector.getClosestPoints(input, bridge, 0);
        contacts.flush();
        return;
    }
    if (in.type0 != kTriangleMesh && in.type1 != kTriangleMesh) {
        // handleCollisionPair, both convex (box and cylinder): the shapes' own
        // implicit dimensions, the algorithm's margins
        contacts.setContactInfo(reinterpret_cast<btPersistentManifold*>(&mf), mf.ea, in.root0, in.root1,
                                in.rest0, in.rest1, in.fric0, in.fric1, false);
        const GuestConvex c0(m, in.shape0, in.type0), c1(m, in.shape1, in.type1);
        convex_convex(in, mf, contacts, c0.ptr(), c0.type, c1.ptr(), c1.type);
        contacts.flush();
        return;
    }
    // handleCollisionPair: convex first, concave second
    if (in.type0 == kTriangleMesh) {
        std::swap(in.shape0, in.shape1);
        std::swap(in.type0, in.type1);
        std::swap(in.world0, in.world1);
        std::swap(in.margin0, in.margin1);
        in.swapped = true;
    }
    convex_concave(m, in, mf, contacts);
    // handleCollisionPair's flush. The contact info -- the manifold among it --
    // is set per triangle, so a pair whose box met no triangle's node is
    // neither refreshed nor written, as on the SPU.
    contacts.flush();
}

template <typename F>
void for_each_pair(const Mem& m, uint32_t desc_ea, F f)
{
    const uint32_t pages = r16(m, desc_ea + L::DescNumPages), last = r32(m, desc_ea + L::DescNumOnLastPage);
    uint32_t in = r32(m, desc_ea + L::DescInPtr);
    for (uint32_t page = 0; page < pages; page++, in += kWorkUnitPage) {
        const uint32_t units = page + 1 < pages ? 1u : last;
        for (uint32_t u = 0; u < units; u++) {
            const uint32_t wu = in + u * 16u;
            const uint32_t pairs = r32(m, wu + 4);   // u64 pair array: its low word
            const int start = (int)r32(m, wu + 8), end = (int)r32(m, wu + 12);
            for (int p = start; p < end; p++) f(pairs + (uint32_t)p * 16u);
        }
    }
}

}  // namespace

int supported(const Mem& m, uint32_t desc_ea, char* why, size_t why_len)
{
    auto no = [&](const char* fmt, uint32_t a, uint32_t b) {
        if (why) snprintf(why, why_len, fmt, a, b);
        return 0;
    };
    const uint32_t pages = r16(m, desc_ea + L::DescNumPages), last = r32(m, desc_ea + L::DescNumOnLastPage);
    if (!pages || last != 1) return no("work-unit pages %u, %u on the last", pages, last);
    int ok = 1;
    for_each_pair(m, desc_ea, [&](uint32_t pe) {
        if (!ok) return;
        const uint32_t alg = r32(m, pe + 8);
        if (r32(m, pe + 12) != 2 || !alg || !r32(m, pe) || !r32(m, pe + 4)) return;
        const uint32_t t0 = r32(m, alg + L::AlgType0), t1 = r32(m, alg + L::AlgType1);
        auto convex = [](uint32_t t) { return t == kBox || t == kCylinder; };
        if (!(convex(t0) || t0 == kTriangleMesh) || !(convex(t1) || t1 == kTriangleMesh) ||
            (t0 == kTriangleMesh && t1 == kTriangleMesh)) {
            ok = no("shape types %u/%u", t0, t1);
            return;
        }
        // each object's shape is the algorithm's type; a cylinder's up axis is one of the three
        for (int k = 0; k < 2 && ok; k++) {
            const uint32_t t = k ? t1 : t0, shape = r32(m, r32(m, alg + (k ? L::AlgObj1 : L::AlgObj0)) + L::ObjShape);
            if (r32(m, shape + L::ShapeType) != t) ok = no("object shape type %u, algorithm's %u", r32(m, shape + L::ShapeType), t);
            else if (t == kCylinder && r32(m, shape + L::CylUpAxis) > 2) ok = no("cylinder up axis %u at +0x%X", r32(m, shape + L::CylUpAxis), L::CylUpAxis);
        }
        if (!ok || (t0 != kTriangleMesh && t1 != kTriangleMesh)) return;
        const uint32_t mesh = r32(m, r32(m, alg + (t0 == kTriangleMesh ? L::AlgObj0 : L::AlgObj1)) + L::ObjShape);
        const uint32_t bvh = r32(m, mesh + L::MeshBvh), iface = r32(m, mesh + L::MeshInterface);
        if (r32(m, bvh + L::BvhVersion) != 273 || !m.base[bvh + L::BvhUseQuant] || r32(m, bvh + L::BvhTraversal) != 0) {
            ok = no("BVH version %u, traversal %u", r32(m, bvh + L::BvhVersion), r32(m, bvh + L::BvhTraversal));
            return;
        }
        if (r32(m, iface + L::IfaceMeshCount) == 1) {
            const uint32_t im = r32(m, iface + L::IfaceMeshes), type = r32(m, im + L::ImIndexType);
            if ((type != kPhyShort && type != kPhyInteger) || r32(m, im + L::ImVertexStride) < 12) {
                ok = no("index type %u, vertex stride %u", type, r32(m, im + L::ImVertexStride));
                return;
            }
        }
    });
    return ok;
}

void run(const Mem& m, uint32_t desc_ea, Result& out, bool write)
{
    Ctx ctx = { &m, &out, write, 0 };
    Ctx* saved = t_ctx;
    t_ctx = &ctx;
    for_each_pair(m, desc_ea, [&](uint32_t pe) { pair(m, pe); });
    t_ctx = saved;
}

void manifolds(const Mem& m, uint32_t desc_ea, std::vector<ManifoldOut>& out)
{
    for_each_pair(m, desc_ea, [&](uint32_t pe) {
        const uint32_t alg = r32(m, pe + 8);
        if (r32(m, pe + 12) != 2 || !alg) return;
        ManifoldOut o;
        o.ea = r32(m, alg + L::AlgManifold);
        o.types = r32(m, alg + L::AlgType0) << 8 | r32(m, alg + L::AlgType1);
        memcpy(o.bytes, m.base + o.ea, kManifoldBytes);
        out.push_back(o);
    });
}

}  // namespace bullet_cd
