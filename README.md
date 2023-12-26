__Ansel__ is a better future for Darktable, designed from real-life use cases and solving actual problems,
by the guy who did the scene-referred workflow and spent these past 4 years working full-time on Darktable.

[Website](https://ansel.photos) (including docs, workflows, support, etc.)

It is forked on Darktable 4.0, and is compatible with editing histories produced with Darktable 4.0 and earlier.
It is not compatible with Darktable 4.2 and later and will not be, since 4.2 introduces irresponsible choices that
will be the burden of those who commited them to maintain, and 4.4 will be even worse.

30.000 lines of code have been removed from Darktable 4.0 and 10.000 lines have been re-written. The end goal
is :

1. to produce a more robust and faster software, with fewer opportunities for weird, contextual bugs
that can't be systematically reproduced, and therefore will never be fixed,
2. to break with the trend of making Darktable a Vim editor for image processing, truly usable
only from (broken) keyboard shortcuts known only by the hardcore geeks that made them,
3. to sanitize the code base in order to reduce the cost of maintenance, now and in the future,
4. to make the general UI nicer to people who don't have a master's in computer science and
more efficient to use for people actually interested in photography, especially for folks
using Wacom (and other brands) graphic tablets,
5. optimize the GUI to streamline the scene-referred workflow and make it feel more natural.

Ultimately, the future of Darktable is [vkdt](https://github.com/hanatos/vkdt/), but
this will be available only for computers with GPU and is a prototype that will not be usable by a general
audience for the next years to come. __Ansel__ aims at sunsetting __Darktable__ with something "finished",
pending a VKDT version usable by common folks.

## Design choices

### Users should not have to read the manual

_(some restrictions apply)_

Image processing is hard. It uses notions of optics and color "science". No matter if you shoot
digital or analog, _illuminant_, _dynamic range_, _gamut_ and _chroma_ will affect your process,
in ways you may not have foreseen, and it might be a good idea to understand what they mean and
where they come at play. Digital has its own lot of issues, from _color spaces & management_ to
_alpha compositing_. Not much we can do here, except providing documentation : you need the skills.
But that's at least the core of what we do.

Managing files and navigating in a graphical interface are things computer users
have been doing for decades, using well-known paradigms that converged to pretty unified semantics.
Users should not have to read a manual to discover why mouse scrolling is blocked,
for example, or how to increase the opacity of a mask, or even what all those silly custom-drawn icons mean.

Users should not have to read the manual because, anyway, they won't. Instead, they will annoy developers
with questions already answered somewhere on the extensive docs, which are too long to read because
they have to explain why too much standard stuff is not handled in a standard way.

Acknowleging that, bad design loses the time of both users and developers, and it's time to cut the losses,
for everybody's sake.

### If it ain't broken, don't fix it

Too much of Darktable "design" has started with "it would be cool if we could ...".
I'll tell you what's cool : hanging good pictures of yours on your walls ASAP.
Visual arts are not performing art (like music or theater), so only the result matters.
Everything that comes before is overhead, and you typically want to keep it minimal.
That's not to say that the process can't be enjoyed in itself.
However, to enjoy the process, you need to master your tools and to bend them to __your__ will,
otherwise you only fight them and the whole process amounts to frustration.
Problem is, Darktable "design" puts too much effort into being different for the sake of it.

In this process of adding "cool new stuff", Darktable has broken keyboard shortcuts and a
lot of basic GUI behaviours, replacing clean code with spaghetti and adding more GUI clutter
without ever pruning stuff.

__Ansel__ has an [explicit](https://github.com/aurelienpierreeng/ansel/wiki/Contributing-to-Ansel#design-process)
design process that mandatorily starts with defined problems met by defined users. Turns
out the quantity of code to write is inversly proportionnal to the amount of thinking you
have done on your solution, typically to spot the root problem out of what users tell you,
and find the simplest path to solution (which is often not even a software solution...).

But bugs don't wait for you in the thinking, they wait only in the code you wrote. So, the more
you think, the less you code, the less maintainance burden you create for yourself in the future.
But of course... you need to have enough time to think things through.
Essentially, that means bye bye to Saturday-afternoon, amateur-driven hacking !

### Don't extend it if you can't simplify it first

A lot of Darktable hacking has been done by copy-pasting code, from other parts of the software, or even
from other projects, mostly because contributors don't have time nor skills to undertake large rewrites.
This triggers code duplication and increases the length of functions,
adding internal branching and introducing `if` and `switch case` nested sometimes on more than 4 levels,
making the structure and logic more difficult to grasp and bugs more difficult (and frustrating) to chase,
while being more likely to happen.

In any case, when the code responsible for existing features is only growing (sometimes by a factor 10 over 4 years),
it raises serious questions regarding future maintainablity, in a context where contributors stick around for
no more than a couple of years, and developers have a limited time to invest. It's simply irresponsible,
as it sacrifices long-term maintainability for shiny new things.

Simplifying and generalizing code, through clean APIs, before adding new features is a must and Ansel
only accepts code I personaly understand and have the skills to maintain. KISS.

## Build and test

If you plan on contributing, you way want to clone the whole Git repository with history, using:

```bash
git clone --recurse-submodules https://github.com/aurelienpierreeng/ansel.git
```

If you just want the current code without plan on contributing, the following command should save
you a lot of bandwidth by discarding 12 years of history:

```bash
git clone --recurse-submodules --depth 1 https://github.com/aurelienpierreeng/ansel.git
```

If you already have cloned the code, and just want to update it with latest changes,
such that it already download the changes (that might save lots of bandwidth), run:

```bash
git pull --recurse-submodules
```

The list of dependencies you need depends on your OS. The most straightforward way is to start a test build,
using the Mac/Linux script provided here :

```bash
sh build.sh --build-type Release --install --sudo --clean-all
```

If a dependency is missing, the script will report it and you just have to locate it in your package manager.
Alternatively, the scripts used to build Ansel by the continuous integration bots can be found in the subfolder
`.github/workflows/*.yml` for Linux Ubuntu, Mac OS and Windows 10 and 11. The `packaging` folder
also contains useful info on how to build for Windows.

## Download and test

The virtual `0.0.0` [pre-release](https://github.com/aurelienpierreeng/ansel/releases/tag/v0.0.0)
contains nightly build, with Linux `.Appimage` and Windows `.exe`, compiled automatically
each night with the latest code, and containing all up-to-date dependencies.

## OS support

Ansel is developped on Fedora, heavily tested on Ubuntu and fairly tested on Windows.

Mac OS and, to a lesser extent, Windows have known GUI issues that come from using Gtk as
a graphical toolkit. Not much can be done here, as Gtk suffers from a lack of Windows/Mac devs too.
Go and support this project so it can have more man-hours put on fixing those.

Mac OS support is not active, the main reason is only 4% of the Darktable user base runs it
while the continuous integration bot for Mac OS needs weekly maintenance (for breaking unexpectedly).
It may or may not work, but if it doesn't, I don't own a Mac box to have a look, so don't expect anything.
Mac OS is anyway not playing nice with Open-Source, having deprecated OpenCL and all.
