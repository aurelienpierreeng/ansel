---
name: Bug Report
about: Open a new bug report
title: ''
labels: ''
assignees: ''
---

## Description of the bug

<!-- Ensure it is not a new design first. -->


**To Reproduce**

<!-- Please provide detailed steps to reproduce the behaviour, for example: -->

1. Go to '...'
2. Click on '...'
3. Scroll down to '...'
4. See error

## Expected behavior


## Context

<!-- You may attach an affected RAW + XMP processing to help debugging -->

**Screenshots**
<!-- if applicable -->

**Screencast**
<!-- if applicable -->


## Which commit introduced the error

<!--
If possible, please try using `git bisect` to determine which commit introduced the issue and place the result here.
A bisect is much appreciated and can significantly simplify the developer's job.
If you don't know how to do it and if you use pre-built packages, provide the name of the version/package name.
-->


## System

<!-- Please fill as much information as possible in the list given below. Please state "unknown" where you do not know the answer and remove any sections that are not applicable -->

* darktable version : e.g. 3.5.0+250~gee17c5dcc
* OS                : e.g. Linux - kernel 5.10.2 / Win10 (Patchlevel) / OSx
* Linux - Distro    : e.g. Ubuntu 18.4
* Memory            :
* Graphics card     :
* Graphics driver   :
* OpenCL installed  :
* OpenCL activated  :
* Xorg              :
* Desktop           :
* GTK+              :
* gcc               :
* cflags            :
* CMAKE_BUILD_TYPE  :

**Additional context**

<!-- Please provide any additional information you think may be useful, for example: -->

 - Can you reproduce with another darktable version(s)? **yes with version x-y-z / no**
 - Can you reproduce with a RAW or Jpeg or both? **RAW-file-format/Jpeg/both**
 - Are the steps above reproducible with a fresh edit (i.e. after discarding history)? **yes/no**
 - If the issue is with the output image, attach an XMP file if (you'll have to change the extension to `.txt`)
 - Is the issue still present using an empty/new config-dir (e.g. start darktable with --configdir "/tmp")? **yes/no**
 - Do you use lua scripts?
   - What lua scripts start automatically?
   - What lua scripts were running when the bug occurred?
