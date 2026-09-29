# Color and color spaces

> **Verified against `f37105c227` on 2026-09-29.** That pass found 2 claim(s) in this file
> that were wrong of the tree and 5 that had gone stale; the ones corrected since carry a note
> saying what they used to say. Anything not yet corrected is flagged inline. Re-measure before
> acting on a claim older than the code you are changing, and re-date this line when you do.

[TOC]

Along the pipeline, we use different color spaces, depending on what we want to represent and how we want to manipulate pixels. Color spaces are chosen for their properties regarding the task to achieve, and it should not be assumed that, because a GUI slider works in Ych or Lch (for example), the pixel operation will (or should) run in those spaces.

## A brief history of Darktable

For a raw picture, the legacy pipeline prior to Darktable 2.6 was designed as follow:

- prior to `colorin`, pixels were in camera RGB (linear), with white point implicitely expected to be D65
- between `colorin` and `colorout`, pixels were in CIE Lab 1976, with white point expected to be D50 (as per ICC standard),
- after `colorout`, pixels were in display RGB (non-linear), with white point typically set to D65 (but depending on display ICC profile).

At that point, modules could not be re-ordered, so checking for in/out color spaces was not needed. Some modules happened after `colorout`, for example [watermark](@ref /src/iop/watermark.c) and [borders](@ref /src/iop/borders.c), probably because they take HSL input implicitely from sRGB, so putting them that late allowed to lazily avoid color spaces conversions (lazily assuming that output color space is sRGB). Of course, if you export to Adobe RGB, the color coordinates are kept as-is, without conversion, so your border or watermark colors will not look the same (more or less saturated, and slightly shifted in hue) depending on the output color profile.

The rationale for using CIE Lab 1976 was to provide decoupled controls over lightness and chromaticity. This is a workflow-wise clever choice, the only problem is Lab is not HDR-able and has a notorious hue linearity issue in the blue-purple region. With modern cameras (starting with Nikon D810 series, back in 2013) boasting 14 EV of dynamic range at 64 ISO, and allowing to shoot backlit scene with post-processing shadows recovery, Lab really did not play nice : shadows dramatically raised took a color shift to a greyish-blue, which didn't live up to the expectation (and didn't compare well with results obtained out of Adobe Lightroom).

It should be noted that it is tremendously difficult to make image manipulation filters work well and reliably in CIE Lab (or in any perceptual color space, even the Darktable UCS 22) :

- masking and blending partial image corrections is not going to work "as is", because the alpha occlusion/compositing model relies on additive light radiometrically-encoded (that is, "linear RGB"), so fringes and edge issues are super hard to avoid near the edges of the mask. Guided filters can, to some extent, alleviate that but they don't solve 100% of the issue and still have an hard time around very contrasted edges (like a dark mountain against backlit sunset),
- lens aberrations like fringes around edges (chromatic aberrations) may actually produce achromatic edges fringes (red CA over blue sky ~= grey), so increasing saturation (as per the CIE definition of saturation : colorfulness judged in proportion of brightness, so a ratio chroma / brightness) may darken both sides of the fringe (blue sky and greenish tree branches, for example), but leave an abnormally bright fringe at the interface.

Though perceptual spaces may be nice from an UI/UX point of view (color operations will be intuitively understood in the lightness/hue/chroma framework), and slightly more predictable, they are generally impossible to blend properly, mitigation technics are typically computationally-expensive, and the side-effects are actually very difficult to predict and understand.

That leaves us with RGB, preferably radiometrically-encoded, which blends robustly with alpha compositing and is physically-consistent with natural phenomena… but is really not intuitive, and which effects are difficult to predict. Just like mixing paints is not mixing colors, RGB is mixing lights, and it's still not mixing colors.

As of Darktable 3.0, the ability to move IOP modules across the pipeline was added, along with on-demand color spaces conversions between modules (allowing to insert RGB modules in the middle of Lab ones), which allowed to write RGB modules destined to go between `colorin` and `colorout`. Properly optimized color conversions RGB <-> Lab are actually surprisingly fast (couple of dozens of ms). This started the scene-referred workflow, which enabled to process most of the editing steps – notoriously, the most critical — in radiometrically-encoded RGB.

## Choosing what color space to work in

The scene-referred workflow is built on the premisses that we post-process an ideal master edit, and this edit will get corrected for output media. As such, we work in unbounded, large-gamut, radiometrically-encoded RGB inside the pipeline. By default, it is ITU-BT.Rec2020. Compared to even-larger spaces (ProPhoto RGB, ACES P0), this one has the benefit of having no imaginary colors, which makes it a bit safer.

Rec2020 is a display space and, as such, has poor perceptual uniformity. Granted, perceptual uniformity is not the target of RGB color spaces, but some weigh hues a bit more evenly. In [_Color saturation control for the 21th century_](https://eng.aurelienpierre.com/2022/02/color-saturation-control-for-the-21th-century/), I have shown how turning typical display RGB to HSV or HSL gives way too much space to the green and purple regions, at the expense of yellow and blue. Controlling color in those spaces may be difficult because of those vanishing hues, so the strength of adjustments may not be uniform across hues.

Better hue uniformity is the design requirement for the Yrg space introduced by Kirk at Filmlight. It is used in Ansel in the color balance RGB module, and also in filmic for the gamut mapping part. It has some imaginary colors though.

For anything affecting color saturation, you have to resort to perceptual spaces (Uniform Color Spaces or Color Appearance Models). [_Color saturation control for the 21th century_](https://eng.aurelienpierre.com/2022/02/color-saturation-control-for-the-21th-century/), I have shown why I believe chroma changes to be caricatural and mostly useless : increasing chroma at constant lightness turns color "fluorescent" (whene evaluated against their surrounding) at some point, at that point is not reached at the same amount of chroma for all hues (Helmholtz-Kohlrausch effect). To account for that, you need to darken them by some amount, along with increasing colorfulness. That's the design premisses of the Darktable UCS 22, with the aforementionned drawback: since the amount of darkening is a function of the original colorfulness, achromatic fringes between 2 saturated regions may become abnormally brighter than their surrounding.

Then again, because a module does work in a certain space does not automatically imply that its GUI should be in the same space. GUI controls will often benefit from UCS, for example when setting the illuminant color, even though the pixel operations work in radiometrically-scaled RGB space.

To choose a color space to write pixel filters in, you need to ask yourself in what space your problem occurs or makes sense:

- optical phenomenon will be better modeled, simulated and reverted, in radiometric large-gamut RGB,
- color grading will be better behaved (from the user setting perspective) in perceptual spaces, but will be better blended and masked in RGB spaces, so Kirk Yrg is an interesting trade-off,
- dyes and inks issues (like prints fading & aging) will be better corrected in CYM(K) spaces.

## Practical implementation

See: <https://www.filmlight.ltd.uk/pdf/whitepapers/FL-TL-TN-0417-StdColourSpaces.pdf>
