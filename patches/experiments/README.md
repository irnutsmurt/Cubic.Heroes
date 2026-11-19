# Experiments kept as patches

Changes tried, measured and not adopted, kept because they may be useful later. None is part of the build:
`tools/pipeline.py` applies only `patches/ps3recomp/`. Each folder holds a diff for the ps3recomp tree (with the
project's patch series applied: `build/ref/ps3recomp`) and one for this repository, made against the commit named
below; the results and why they were not adopted are in the document named.

To try one: `git -C build/ref/ps3recomp apply <dir>/ps3recomp-rsx_vulkan_backend.diff` and
`git apply <dir>/port-ppfx_render.diff`, rebuild, and give the game a Vulkan 1.1 instance (`PS3RECOMP_VK_GPL=1
DOTHEROES_GPL=0`: the backend then asks the loader for its own version without using pipeline libraries). Revert with
`git checkout` of the two files.

## m11_4-depth-bias-control (M11.4, made against 4030edb)

On a device without D24_UNORM_S8 (AMD's RADV) every depth surface is D32_SFLOAT_S8, whose constant depth bias is a
step of the primitive's largest depth, not the RSX's one 2^-24 step. VK_EXT_depth_bias_control (the
least-representable-value representation forced to UNORM, exact) gives the 2^-24 step on the float surface; the
renderer sets it with vkCmdSetDepthBias2EXT on D32 surfaces only.

Result on the Renoir iGPU (docs/M11_4_PLAN.md section 26): the king's shadow map's mean difference from RPCS3 2.8e-6
-> 6.7e-7, its texels within two 2^-24 steps 59% -> 63%; the corridor and entry hall unchanged. Not adopted: it needs a
Vulkan 1.1 instance (every system's start changes; the instance is 1.0 by design) for a small gain. Worth revisiting
with M12, or if the instance moves to 1.1 for another reason.

## m11_4-rectangular-lines (M11.4, made against 4030edb)

RADV's lines are not strict (strictLines false: parallelograms); NVIDIA's are, and RPCS3's references were matched
with them. VK_EXT_line_rasterization's rectangularLines, asked for asp's line pipelines where the device's lines are
not strict, would draw the editor's grid as a strict device does.

Not measured in effect: with the default Vulkan 1.0 instance the backend never enables it (the feature query needs
vkGetPhysicalDeviceFeatures2). The difference it addresses: the editor scene's final image, blue channel mean 1.1
against the accepted 0.018, 0.19% of pixels (docs/M11_4_PLAN.md section 26). Not adopted for the same instance
requirement.
