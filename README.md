# Ansel

[TOC]

## What is it ?

__Ansel__ is a better future for Darktable, designed from real-life use cases and solving actual problems,
by the guy who did the scene-referred workflow and spent these past 4 years working full-time on Darktable.

It is forked on Darktable 4.0, and is compatible with editing histories produced with Darktable 4.0 and earlier.
It is not compatible with Darktable 4.2 and later and will not be, since 4.2 introduces irresponsible choices that
will be the burden of those who commited them to maintain, and 4.4 will be even worse.

The end goal is :

1. to produce a more robust and faster software, with fewer opportunities for weird, contextual bugs
that can't be systematically reproduced, and therefore will never be fixed,
2. to break with the trend of making Darktable a Vim editor for image processing, truly usable
only from (broken) keyboard shortcuts known only by the hardcore geeks that made them,
3. to sanitize the code base in order to reduce the cost of maintenance, now and in the future,
4. to make the general UI nicer to people who don't have a master's in computer science and
more efficient to use for people actually interested in photography, especially for folks
using Wacom (and other brands) graphic tablets,
5. to optimize the GUI to streamline the scene-referred workflow and make it feel more natural.

Ultimately, the future of Darktable is [vkdt](https://github.com/hanatos/vkdt/), but
this will be available only for computers with GPU and is a prototype that will not be usable by a general
audience for the next years to come. __Ansel__ aims at sunsetting __Darktable__ with something "finished",
pending a VKDT version usable by common folks.

## Download and test

The virtual `0.0.0` [pre-release](https://github.com/aurelienpierreeng/ansel/releases/tag/v0.0.0)
contains nightly builds, with Linux `.Appimage`, Windows `.exe` and Mac OS `.dmg`, compiled automatically
each night with the latest code, and containing all up-to-date dependencies.

Ansel is in alpha version. The GUI is susceptible to change and the application may crash under some circumstances.

## OS support

Ansel is developped on Ubuntu and Windows.

Mac OS and, to a lesser extent, Windows have known GUI issues that come from using Gtk as
a graphical toolkit. Not much can be done here, as Gtk suffers from a lack of Windows/Mac devs too.
Go and support these projects so they can have more man-hours put on fixing those.

## Useful links

- [User documentation](https://ansel.photos/en/doc/), in particular:
    - [Build and test on Linux](https://ansel.photos/en/doc/install/linux)
    - [Build and test on Windows](https://ansel.photos/en/doc/install/windows)
- [Contributing guidelines](https://ansel.photos/en/contribute/), in particular:
    - [Project organization](https://ansel.photos/en/contribute/organization/)
    - [Translating](https://ansel.photos/en/contribute/translating/)
    - [Coding style](https://ansel.photos/en/contribute/coding-style/)
- [Developer documentation](https://dev.ansel.photos)
- [Project news](https://ansel.photos/en/news/)
- Community forum - the community forum was hacked, we are looking into setting up a new one
- [Matrix chatrooms](https://app.element.io/#/room/#ansel:matrix.org)
- [Support](https://ansel.photos/en/support/)

## Code analysis

Ansel was forked from Darktable after commit 7b88fdd7afe7b8530a992ae3c12e7a088dc9e992, 1 month before Darktable 4.0 release
(output truncated to relevant languages):

```
$ cloc --git --diff 7b88fdd7afe7b8530a992ae3c12e7a088dc9e992 HEAD
github.com/AlDanial/cloc v 2.02  T=227.18 s (2.9 files/s, 4772.1 lines/s)
--------------------------------------------------------------------------------
Language                      files          blank        comment           code
--------------------------------------------------------------------------------
C
 same                             0              0          23250         199103
 modified                       276              0           1304          10590
 added                           27           2848           3952          24289
 removed                         50           7419           5941          60296
C/C++ Header
 same                             0              0           5387          13744
 modified                       112              0            236            673
 added                           11            497           1535           1869
 removed                         12            376            956           3072
C++
 same                             0              0           1018           8555
 modified                         9              0             22            200
 added                            0             15             52            370
 removed                          0             11             14            181
CSS
 same                             0              0              0              0
 modified                         0              0              0              0
 added                            3            324            357           1794
 removed                         13            419            519           9575
...
--------------------------------------------------------------------------------
SUM:
 same                             3             62          53088         466590
 modified                       533              0         109729          57014
 added                           81          11482          38366         116769
 removed                        181          31887         103914         211349
--------------------------------------------------------------------------------
```

```
$ cloc --git 7b88fdd7afe7b8530a992ae3c12e7a088dc9e992
github.com/AlDanial/cloc v 2.02  T=3.70 s (286.4 files/s, 327937.4 lines/s)
--------------------------------------------------------------------------------
Language                      files          blank        comment           code
--------------------------------------------------------------------------------
C                               439          51319          37122         303148
C/C++ Header                    284           7286          11522          24848
C++                              11           1489           1138           9899
CSS                              20            564            617          10353
...
--------------------------------------------------------------------------------
SUM:                           1160         221925         280733         832393
--------------------------------------------------------------------------------
```

```
$ cloc --git HEAD
github.com/AlDanial/cloc v 2.02  T=3.70 s (286.4 files/s, 327937.4 lines/s)
--------------------------------------------------------------------------------
Language                      files          blank        comment           code
--------------------------------------------------------------------------------
C                               416          46748          35133         267141
C/C++ Header                    284           7416          12115          23674
C++                              11           1493           1176          10088
CSS                              10            488            443           2555
...
--------------------------------------------------------------------------------
SUM:                           1061         201521         230847         769883
--------------------------------------------------------------------------------
```

The volume of C code has therefore been reduced by 11%, the volume of CSS (for theming)
by 75%. Excluding the pixel operations (`cloc  --fullpath --not-match-d=/src/iop --git`),
the C code volume has reduced by 15%.

The [cyclomatic complexity](https://en.wikipedia.org/wiki/Cyclomatic_complexity) of the project
has also been reduced:

| Metric | Ansel Master | Darktable 4.0 | Darktable 5.0 |
| ------ | -----------: | ------------: | ------------: |
| Cyclomatic complexity | [48,424](https://sonarcloud.io/component_measures?metric=complexity&id=aurelienpierreeng_ansel) | [56,170](https://sonarcloud.io/component_measures?metric=complexity&id=aurelienpierre_darktable) | [59,377](https://sonarcloud.io/component_measures?metric=complexity&id=aurelienpierreeng_darktable-5) |
| Cognitive complexity | [60,664](https://sonarcloud.io/component_measures?metric=cognitive_complexity&id=aurelienpierreeng_ansel) | [72,743](https://sonarcloud.io/component_measures?metric=cognitive_complexity&id=aurelienpierre_darktable) | [77,039](https://sonarcloud.io/component_measures?metric=cognitive_complexity&id=aurelienpierreeng_darktable-5) |
| Lines of code | [326,190](https://sonarcloud.io/component_measures?metric=ncloc&id=aurelienpierreeng_ansel) | [361,046](https://sonarcloud.io/component_measures?metric=ncloc&id=aurelienpierre_darktable) | [370,781](https://sonarcloud.io/component_measures?metric=ncloc&id=aurelienpierreeng_darktable-5) |
| Ratio of comments | [12.2%](https://sonarcloud.io/component_measures?metric=comment_lines_density&id=aurelienpierreeng_ansel) | [11.5%](https://sonarcloud.io/component_measures?metric=comment_lines_density&id=aurelienpierre_darktable) | [11.7%](https://sonarcloud.io/component_measures?metric=comment_lines_density&id=aurelienpierreeng_darktable-5) |

Those figures are indirect indicators of the long-term maintainability of the project:

- comments document the code and are used by Doxygen to build the [dev docs](https://dev.ansel.photos),
- code volume and complexity make bugs harder to find and fix properly, and lead to more cases to cover with tests,
- code volume and complexity prevent from finding optimization opportunities,
- let's remember that it's mostly the same software with pretty much the same features anyway.

Dealing with growing features should be made through modularity, that is splitting the app features into modules,
enclosing modules into their own space, and make modules independent from each other's internals.
We will see below how those "modules" behave (spoiler 1: that this did not happen), (spoiler 2:
feel free to go the [dev docs](https://dev.ansel.photos), where all functions have their
dependency graph in their doc, to witness that "modules" are not even modular, and the whole application
is actually aware of the whole application).

Let's see a comparison of Ansel vs. Dartable 4.0 and 5.0 complexity per file/feature
(figures are: cyclomatic complexity / lines of code excluding comments - lower is better) :


### Pixel pipeline, development history, image manipulation backends

| File | Description | Ansel Master  | Darktable 4.0 | Darktable 5.0 |
| ---- | ----------- | ------------: | ------------: | ------------: |
| `src/common/selection.c` | Database images selection backend | [59](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fcommon%2Fselection.c&view=list&id=aurelienpierreeng_ansel) / 259 | [62](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fcommon%2Fselection.c&view=list&id=aurelienpierre_darktable) / 342 | [62](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fcommon%2Fselection.c&view=list&id=aurelienpierreeng_darktable-5) / 394 |
| `src/common/act_on.c` | GUI images selection backend | [12](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fcommon%2Fact_on.c&view=list&id=aurelienpierreeng_ansel) / 35 | [80](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fcommon%2Fact_on.c&view=list&id=aurelienpierre_darktable) / 297 | [80](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fcommon%2Fact_on.c&view=list&id=aurelienpierreeng_darktable-5) / 336 |
| `src/common/collection.c` | Image collection extractions from library database | [397](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fcommon%2Fcollection.c&view=list&id=aurelienpierreeng_ansel) / 1565 | [532](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fcommon%2Fcollection.c&view=list&id=aurelienpierre_darktable) / 2133 | [553](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fcommon%2Fcollection.c&view=list&id=aurelienpierreeng_darktable-5) / 2503 |
| `src/develop/pixelpipe_hb.c` | Pixel pipeline processing backbone (original) |- | [473](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fdevelop%2Fpixelpipe_hb.c&view=list&id=aurelienpierre_darktable) / 2013 | [553](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fdevelop%2Fpixelpipe_hb.c&view=list&id=aurelienpierreeng_darktable-5) / 2644 |
| `src/develop/pixelpipe_hb.c` | Pixel pipeline processing backbone (refactored) | [404](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fdevelop%2Fpixelpipe_hb.c&view=list&id=aurelienpierreeng_ansel) / 1827 | - | - |
| `src/develop/dev_pixelpipe.c` | Pipeline/development interface (refactored from `pixelpipe_hb.c`) | [105](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fdevelop%2Fdev_pixelpipe.c&view=list&id=aurelienpierreeng_ansel) / 381 | - | - |
| `src/develop/pixelpipe_cache.c` | Pixel pipeline image cache* | [89](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fdevelop%2Fpixelpipe_cache.c&view=list&id=aurelienpierreeng_ansel) / 415 | [55](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fdevelop%2Fpixelpipe_cache.c&view=list&id=aurelienpierre_darktable) / 242 | [95](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fdevelop%2Fpixelpipe_cache.c&view=list&id=aurelienpierreeng_darktable-5) / 368 |
| `src/common/imageio.c` | Backend for export and thumbnail pipelines | [213](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fcommon%2Fimageio.c&view=list&id=aurelienpierreeng_ansel) / 1000 | [218](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fcommon%2Fimageio.c&view=list&id=aurelienpierre_darktable) / 1129 | [215](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fimageio%2Fimageio.c&view=list&id=aurelienpierreeng_darktable-5) / 1787 |
| `src/common/mipmap_cache.c` | Lighttable thumbnails cache | [180](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fcommon%2Fmipmap_cache.c&view=list&id=aurelienpierreeng_ansel) / 959 | [193](https://sonarcloud.io/component_measures?metric=ncloc&selected=aurelienpierre_darktable%3Asrc%2Fcommon%2Fmipmap_cache.c&id=aurelienpierre_darktable) / 1048 | [225](https://sonarcloud.io/component_measures?metric=ncloc&selected=aurelienpierreeng_darktable-5%3Asrc%2Fcommon%2Fmipmap_cache.c&id=aurelienpierreeng_darktable-5) / 1310 |
| `src/develop/develop.c` | Development history & pipeline backend (original) | - | [600](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fdevelop%2Fdevelop.c&view=list&id=aurelienpierre_darktable) / 2426| [628](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fdevelop%2Fdevelop.c&view=list&id=aurelienpierreeng_darktable-5) / 2732 |
| `src/develop/develop.c` | Development pipeline backend (refactored) | [273](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fdevelop%2Fdevelop.c&view=list&id=aurelienpierreeng_ansel) / 1035 | - |  - |
| `src/develop/dev_history.c` | Development history backend (refactored from `develop.c`) | [261](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fdevelop%2Fdev_history.c&view=list&id=aurelienpierreeng_ansel) / 1208 | - | - |
| `src/develop/imageop.c` | Pixel processing module API | [541](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fdevelop%2Fimageop.c&view=list&id=aurelienpierreeng_ansel) / 2287 | [617](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fdevelop%2Fimageop.c&view=list&id=aurelienpierre_darktable) / 2513 | [692](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fdevelop%2Fimageop.c&view=list&id=aurelienpierreeng_darktable-5) / 3181 |
| `src/control/jobs/control_jobs.c` | Background thread tasks | [264](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fcontrol%2Fjobs%2Fcontrol_jobs.c&view=list&id=aurelienpierreeng_ansel) / 1619 | [308](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fcontrol%2Fjobs%2Fcontrol_jobs.c&view=list&id=aurelienpierre_darktable) / 1904 | [394](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fcontrol%2Fjobs%2Fcontrol_jobs.c&view=list&id=aurelienpierreeng_darktable-5) / 2475 |


*: to this day, Darktable pixel pipeline cache is still broken as of 5.0, which clearly shows that increasing its complexity was not a solution:
- https://github.com/darktable-org/darktable/issues/18517
- https://github.com/darktable-org/darktable/issues/18133

### GUI

| File | Description | Ansel Master  | Darktable 4.0 | Darktable 5.0 |
| ---- | ----------- | ------------: | ------------: | ------------: |
| `src/bauhaus/bauhaus.c` | Custom Gtk widgets (sliders/comboboxes) for modules | [483](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fbauhaus%2Fbauhaus.c&id=aurelienpierreeng_ansel) / 2429 | [653](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fbauhaus%2Fbauhaus.c&view=list&id=aurelienpierre_darktable) / 2833 | [751](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fbauhaus%2Fbauhaus.c&view=list&id=aurelienpierreeng_darktable-5) / 3317 |
| `src/gui/accelerators.c` | Key shortcuts handler | [216](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fgui%2Faccelerators.c&view=list&id=aurelienpierreeng_ansel) / 1144 | [1088](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fgui%2Faccelerators.c&view=list&id=aurelienpierre_darktable) / 3546 | [1245](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fgui%2Faccelerators.c&view=list&id=aurelienpierreeng_darktable-5) / 5221 |
| `src/views/view.c` | Base features of GUI views | [220](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fviews%2Fview.c&view=list&id=aurelienpierreeng_ansel) / 795 | [298](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fviews%2Fview.c&view=list&id=aurelienpierre_darktable) / 1105 | [325](https://sonarcloud.io/component_measures?metric=ncloc&selected=aurelienpierreeng_darktable-5%3Asrc%2Fviews%2Fview.c&view=list&id=aurelienpierreeng_darktable-5) / 1865 |
| `src/views/darkroom.c` | Darkroom GUI view | [374](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fviews%2Fdarkroom.c&view=list&id=aurelienpierreeng_ansel) / 2041 | [736](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fviews%2Fdarkroom.c&view=list&id=aurelienpierre_darktable) / 3558 | [560](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fviews%2Fdarkroom.c&view=list&id=aurelienpierreeng_darktable-5) / 2776 |
| `src/libs/modulegroups.c` | Groups of modules in darkroom GUI | [120](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Flibs%2Fmodulegroups.c&view=list&id=aurelienpierreeng_ansel) / 478 | [554](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Flibs%2Fmodulegroups.c&view=list&id=aurelienpierre_darktable) / 3155 | [564](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Flibs%2Fmodulegroups.c&view=list&id=aurelienpierreeng_darktable-5) / 3322 |
| `src/views/lighttable.c` | Lighttable GUI view | [17](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fviews%2Flighttable.c&view=list&id=aurelienpierreeng_ansel) / 152 | [227](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fviews%2Flighttable.c&view=list&id=aurelienpierre_darktable) / 1002 | [237](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fviews%2Flighttable.c&view=list&id=aurelienpierreeng_darktable-5) / 1007 |
| `src/dtgtk/thumbtable.c` | Lighttable & filmroll grid of thumbnails view | [346](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fdtgtk%2Fthumbtable.c&view=list&id=aurelienpierreeng_ansel) / 1465 | [533](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fdtgtk%2Fthumbtable.c&view=list&id=aurelienpierre_darktable) / 2146 | [559](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fdtgtk%2Fthumbtable.c&view=list&id=aurelienpierreeng_darktable-5) / 2657 |
| `src/dtgtk/thumbnail.c` | Lighttable & filmroll thumbnails | [212](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fdtgtk%2Fthumbnail.c&view=list&id=aurelienpierreeng_ansel) / 1038 | [345](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Fdtgtk%2Fthumbnail.c&view=list&id=aurelienpierre_darktable) / 1622 | [332](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Fdtgtk%2Fthumbnail.c&view=list&id=aurelienpierreeng_darktable-5) / 1841 |
| `src/libs/tools/filter.c` | Darktable 3.x Lighttable collection filters & sorting (original) | [85](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Flibs%2Ftools%2Ffilter.c&id=aurelienpierreeng_ansel) / 620 | - | - |
| `src/libs/filters` | Darktable 4.x Lighttable collection filters (modules) | - | [453](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Flibs%2Ffilters&id=aurelienpierre_darktable) / 2296 | [504](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Flibs%2Ffilters&id=aurelienpierreeng_darktable-5) / 2664 |
| `src/libs/filtering.c` | Darktable 4.x Lighttable collection filters (main widget) | - | [245](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Flibs%2Ffiltering.c&id=aurelienpierre_darktable) / 1633 | [290](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Flibs%2Ffiltering.c&view=list&id=aurelienpierreeng_darktable-5) / 1842 |
| `src/common/import.c` | File import popup window | [135](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_ansel%3Asrc%2Fcommon%2Fimport.c&view=list&id=aurelienpierreeng_ansel) / 931 | [309](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierre_darktable%3Asrc%2Flibs%2Fimport.c&view=list&id=aurelienpierre_darktable) / 1923 | [334](https://sonarcloud.io/component_measures?metric=complexity&selected=aurelienpierreeng_darktable-5%3Asrc%2Flibs%2Fimport.c&view=list&id=aurelienpierreeng_darktable-5) / 2309 |


## Runtimes

All runtimes computed on a Lenovo Thinkpad P51 laptop (Intel Xeon CPU E3-1505M v6 @ 3.00GHz, Nvidia GPU Quadro M2200 4 GB vRAM, 32 GB RAM, 4K display), CPU in performance mode, Linux Fedora 41 with KDE/Plasma desktop. Pixel pipeline runtimes are not compared since Ansel 0.1-alpha shares its pixel code with Darktable 4.0 by design (compatibility). Ansel Master is taken at commit d56afa0c (Feb. 4th 2026).

| Description | Ansel Master | Darktable 5.0 |
| ----------- | ------------ | ------------- |
| Time from app startup to last lighttable thumbnail drawing (same collection) | 2.59 s | 7.49 s |
| Time to switch from lighttable to darkroom (same image) | 0.2 s | 1.2 s |
| Time to scroll (start->end) through the same collection of 471 images* | 0.7 s | 5.0 s |

*: thumbnails preloaded in disk cache in both cases, 5 thumbs columns/row, 4K resolution, no right sidebar.

The following have been measured on battery, in powersave mode, with the application sitting idle (no user interaction) for 5 minutes, using Intel Powertop. The baseline consumption of the whole idle OS is 1.6 % CPU. (Power is given for the app only, % CPU is given for the whole system):

| View | Ansel Master | Darktable 5.0 |
| ---- | ------------ | ------------- |
| Lighttable | 1.8 % CPU, power: 0.85 mW | 2.7 % CPU, power: 103 mW |
| Darkroom   | 1.8 % CPU, power: 7.65 mW | 1.8 % CPU, power: 22 mW |

These figures represent the baseline power consumption of the GUI alone (Gtk, background workers, scheduled timers, etc.).

TL;DR: Darktable is leaking performance by the GUI, and the tedious work done in 2023-2024 on optimizing pixel processing modules for an extra 15-50 ms is completely irrelevant.

## Contributors

Github doesn't show anymore the contributions for repositories having more than 10.000 commits…

### Number of commits

#### Since forever

```bash
$ git shortlog -sn --no-merges
  4295  Pascal Obry
  3597  johannes hanika
  2565  Aurélien PIERRE
  2127  Tobias Ellinghaus
  2083  Roman Lebedev
  1757  Henrik Andersson
  1527  Ulrich Pegelow
  1250  Aldric Renaudin
   948  Pascal de Bruijn
   841  Pedro Côrte-Real
   692  Ralf Brown
   604  Jérémy Rosen
   601  Dan Torop
   566  Diederik ter Rahe
   546  Philippe Weyland
   516  Hanno Schwalm
   430  Hubert Kowalski
   355  parafin
   331  Ger Siemerink
   286  Chris.Elston
   283  Jeronimo Pellegrini
   276  rawfiner
   243  Nicolas Auffray
   236  José Carlos García Sogo
   207  Andreas Schneider
   200  EdgarLux
   195  Robert Bieber
   192  Michel Leblond
   183  Richard Levitte
   180  Heiko Bauke
   172  Edouard Gomez
   162  Stefan Schöfegger
   155  edgardoh
   140  Miloš Komarčević
   126  Peter Budai
   122  Victor Forsiuk
   103  Bill Ferguson
   101  Simon Spannagel
   100  Alynx Zhou
    93  Jean-Sébastien Pédron
    90  Martin Straeten
    83  Olivier Tribout
    76  Alexandre Prokoudine
    73  Mark-64
    67  Bruce Guenter
    66  Sakari Kapanen
    61  Timur Davletshin
    55  Matthieu Moy
    55  bartokk
    50  Rostyslav Pidgornyi
    49  Chris Elston
    48  Moritz Lipp
    48  tatica
    44  Dennis Gnad
    43  Marco Carrarini
    42  Christian Tellefsen
    41  Josep V. Moragues
    39  Matt Maguire
    38  Matthieu Volat
    38  Maurizio Paglia
    36  Daniel Vogelbacher
    36  Jakub Filipowicz
    35  Thomas Pryds
    32  David-Tillmann Schaefer
    32  GrahamByrnes
    32  Harold le Clément de Saint-Marcq
    31  Marco
    30  Guillaume Stutin
    30  marcel
    28  wpferguson
    27  HansBull
    27  Kaminsky Andrey
    27  Matjaž Jeran
    25  mepi0011
    25  shlomi braitbart
    24  Michal Babej
    23  lhietal
    22  quovadit
    21  Antony Dovgal
    21  Jacques Le Clerc
    21  Milan Knížek
    20  Sam Smith
    19  Richard Wonka
    19  Ryo Shinozaki
    18  Jiyone
    18  Rikard Öxler
    18  darkelectron
    17  Guillaume Marty
    16  Alexis Mousset
    16  Báthory Péter
    16  Dimitrios Psychogios
    16  José Carlos Casimiro
    16  Wolfgang Goetz
    15  Guilherme Brondani Torri
    15  U-DESKTOP-HQME86J\marco
    15  Vasyl Tretiakov
    15  Vincent THOMAS
    14  Brian Teague
    14  Frédéric Grollier
    14  lologor
    14  luzpaz
    13  Kevin Vermassen
    13  Marcus Gama
    13  Novy Sawai
    13  Philipp Lutz
    13  Robert Bridge
    12  Germano Massullo
    12  James C. McPherson
    12  Victor Lamoine
    11  Andrew Toskin
    11  Eckhart Pedersen
    11  Felipe Contreras
    11  Mikko Ruohola
    10  Asma
    10  Bernd Steinhauser
    10  Kanstantsin Shautsou
    10  Martin Burri
    10  Serkan ÖNDER
    10  Tomasz Golinski
    10  Victor Engmark
    10  Wyatt Olson
    10  junkyardsparkle
    10  thisnamewasnottaken
     9  Arnaud TANGUY
     9  Fabio Heer
     9  JohnnyRun
     9  Loic Guibert
     9  Paolo DePetrillo
     9  Žilvinas Žaltiena
     8  Benoit Brummer
     8  Dušan Kazik
     8  Jan Kundrát
     8  Jochen Schroeder
     8  Matteo Mardegan
     8  Petr Styblo
     8  Robert William Hutton
     8  Roman Khatko
     8  Stuart Henderson
     8  itinerarium
     8  luz paz
     8  vertama
     8  vrnhgd
     7  Ammon Riley
     7  Chris Hodapp
     7  David Bremner
     7  Gaspard Jankowiak
     7  Ivan Tarozzi
     7  Jim Robinson
     7  Marcello Mamino
     7  Marcus Rückert
     7  Richard Hughes
     7  calca
     7  elstoc
     7  篠崎亮　Ryo Shinozaki
     6  Artur de Sousa Rocha
     6  Cherrot Luo
     6  Christian Himpel
     6  Denis Dyakov
     6  Dominik Markiewicz
     6  Guillaume Benny
     6  Harald
     6  Jesper Pedersen
     6  Maximilian Trescher
     6  Petr Stasiak
     6  Pierre Lamot
     6  Sergey Pavlov
     6  Stephan Hoek
     6  Wolfgang Mader
     6  grand-piano
     6  piratenpanda
     6  solarer
     5  August Schwerdfeger
     5  JP Verrue
     5  Johanes Schneider
     5  K. Adam Christensen
     5  Karl Mikaelsson
     5  Luca Zulberti
     5  Matthias Gehre
     5  Matthias Vogelgesang
     5  Simon Legner
     5  Tianhao Chai
     5  Torsten Bronger
     5  matt-maguire
```

#### Since forking Ansel

```bash
$ git shortlog -sn --no-merges --since "JUN 1 2022"
  1576  Aurélien PIERRE
   100  Alynx Zhou
    30  Guillaume Stutin
    24  Hanno Schwalm
    18  Jiyone
    17  Guillaume Marty
    14  lologor
    11  Miloš Komarčević
    10  Sakari Kapanen
     9  Maurizio Paglia
     9  Victor Forsiuk
     6  Pascal Obry
     5  Luca Zulberti
     4  Alban Gruin
     4  Ricky Moon
     3  Sidney Markowitz
     2  Diederik ter Rahe
     2  Marrony Neris
     2  Miguel Moquillon
     2  Roman Neuhauser
     2  parafin
     1  Aldric Renaudin
     1  André Doherty
     1  Chris Elston
     1  Germano Massullo
     1  Jehan Singh
     1  Marc Cousin
     1  Philippe Weyland
     1  Ralf Brown
     1  Roman Lebedev
     1  naveen
     1  realSpok
     1  tatu
```

#### Before forking Ansel (Darktable legacy)

```bash
$ git shortlog -sn --no-merges --before "JUN 1 2022"
  4289  Pascal Obry
  3597  johannes hanika
  2127  Tobias Ellinghaus
  2082  Roman Lebedev
  1757  Henrik Andersson
  1527  Ulrich Pegelow
  1249  Aldric Renaudin
   989  Aurélien PIERRE
   948  Pascal de Bruijn
   841  Pedro Côrte-Real
   691  Ralf Brown
   604  Jérémy Rosen
   601  Dan Torop
   564  Diederik ter Rahe
   545  Philippe Weyland
   492  Hanno Schwalm
   430  Hubert Kowalski
   353  parafin
   331  Ger Siemerink
   286  Chris.Elston
   283  Jeronimo Pellegrini
   276  rawfiner
   243  Nicolas Auffray
   236  José Carlos García Sogo
   207  Andreas Schneider
   200  EdgarLux
   195  Robert Bieber
   192  Michel Leblond
   183  Richard Levitte
   180  Heiko Bauke
   172  Edouard Gomez
   162  Stefan Schöfegger
   155  edgardoh
   129  Miloš Komarčević
   126  Peter Budai
   113  Victor Forsiuk
   103  Bill Ferguson
   101  Simon Spannagel
    93  Jean-Sébastien Pédron
    90  Martin Straeten
    83  Olivier Tribout
    76  Alexandre Prokoudine
    73  Mark-64
    67  Bruce Guenter
    61  Timur Davletshin
    56  Sakari Kapanen
    55  Matthieu Moy
    55  bartokk
    50  Rostyslav Pidgornyi
    48  Chris Elston
    48  Moritz Lipp
    48  tatica
    44  Dennis Gnad
    43  Marco Carrarini
    42  Christian Tellefsen
    41  Josep V. Moragues
    39  Matt Maguire
    38  Matthieu Volat
    36  Daniel Vogelbacher
    36  Jakub Filipowicz
    35  Thomas Pryds
    32  David-Tillmann Schaefer
    32  GrahamByrnes
    32  Harold le Clément de Saint-Marcq
    31  Marco
    30  marcel
    29  Maurizio Paglia
    28  wpferguson
    27  HansBull
    27  Kaminsky Andrey
    27  Matjaž Jeran
    25  mepi0011
    25  shlomi braitbart
    24  Michal Babej
    23  lhietal
    22  quovadit
    21  Antony Dovgal
    21  Jacques Le Clerc
    21  Milan Knížek
    20  Sam Smith
    19  Richard Wonka
    19  Ryo Shinozaki
    18  Rikard Öxler
    18  darkelectron
    16  Alexis Mousset
    16  Báthory Péter
    16  Dimitrios Psychogios
    16  José Carlos Casimiro
    16  Wolfgang Goetz
    15  Guilherme Brondani Torri
    15  U-DESKTOP-HQME86J\marco
    15  Vasyl Tretiakov
    15  Vincent THOMAS
    14  Brian Teague
    14  Frédéric Grollier
    14  luzpaz
    13  Kevin Vermassen
    13  Marcus Gama
    13  Novy Sawai
    13  Philipp Lutz
    13  Robert Bridge
    12  James C. McPherson
    12  Victor Lamoine
    11  Andrew Toskin
    11  Eckhart Pedersen
    11  Felipe Contreras
    11  Germano Massullo
    11  Mikko Ruohola
    10  Asma
    10  Bernd Steinhauser
    10  Kanstantsin Shautsou
    10  Martin Burri
    10  Serkan ÖNDER
    10  Tomasz Golinski
    10  Victor Engmark
    10  Wyatt Olson
    10  junkyardsparkle
    10  thisnamewasnottaken
     9  Arnaud TANGUY
     9  Fabio Heer
     9  JohnnyRun
     9  Loic Guibert
     9  Paolo DePetrillo
     9  Žilvinas Žaltiena
     8  Benoit Brummer
     8  Dušan Kazik
     8  Jan Kundrát
     8  Jochen Schroeder
     8  Matteo Mardegan
     8  Petr Styblo
     8  Robert William Hutton
     8  Roman Khatko
     8  Stuart Henderson
     8  itinerarium
     8  luz paz
     8  vertama
     8  vrnhgd
     7  Ammon Riley
     7  Chris Hodapp
     7  David Bremner
     7  Gaspard Jankowiak
     7  Ivan Tarozzi
     7  Jim Robinson
     7  Marcello Mamino
     7  Marcus Rückert
     7  Richard Hughes
     7  calca
     7  elstoc
     7  篠崎亮　Ryo Shinozaki
     6  Artur de Sousa Rocha
     6  Cherrot Luo
     6  Christian Himpel
     6  Denis Dyakov
     6  Dominik Markiewicz
     6  Guillaume Benny
     6  Harald
     6  Jesper Pedersen
     6  Maximilian Trescher
     6  Petr Stasiak
     6  Pierre Lamot
     6  Sergey Pavlov
     6  Stephan Hoek
     6  Wolfgang Mader
     6  grand-piano
     6  piratenpanda
     6  solarer
     5  August Schwerdfeger
     5  JP Verrue
     5  Johanes Schneider
     5  K. Adam Christensen
     5  Karl Mikaelsson
     5  Matthias Gehre
     5  Matthias Vogelgesang
     5  Simon Legner
     5  Tianhao Chai
     5  Torsten Bronger
     5  matt-maguire
```
