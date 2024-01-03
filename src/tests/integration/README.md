darktable is an open source photography workflow application and
non-destructive raw developer. A virtual lighttable and darkroom for
photographers. It manages your digital negatives in a database, lets
you view them through a zoomable lighttable and enables you to develop
raw images, enhance them and export them on local or remote storage.

This repository hosts the darktable integration tests.

# Structure

```
images/     : a directory containing test images

run.sh      : main driver

deltae      : python script to compute a delta-E between 2 images
              expected.jpg and output.jpg

nnnn-name/  : tests
```

Needed tools : zopflipng


# How to add a new test (using default driver)

1. Create a new directory

```
   <nnnn>-<meaningful name>
```

2. Start darktable, open one test image (or add a new one if needed)

3. Do a dev using whatever module

4. Copy the resulting .xmp into <nnnn>-<meaningful name>

   And rename it ```<meaningful name>.xmp```

5. Do a first run of the test to get the expected output

```bash
   ./run <dir>
```

   The output.png will be copied to expected.png, double check that
   expected.png is correct and really the expected output.

6. Test that all is ok by running:


```bash
   ./run <dir>
```

   All values must be 0 as there is no change in darktable, so the
   expected output should be exactly the same image as the output.

```bash
   ./run.sh 0001-exposure

Test 0001-exposure
      Image mire1.cr2
      CPU & GPU version differ by 25699 pixels
      ----------------------------------
      Max dE                   : 0.00000
      Avg dE                   : 0.00000
      Std dE                   : 0.00000
      ----------------------------------
      Pixels below avg + 0 std : 100.00 %
      Pixels below avg + 1 std : 100.00 %
      Pixels below avg + 3 std : 100.00 %
      Pixels below avg + 6 std : 100.00 %
      Pixels below avg + 9 std : 100.00 %
      ----------------------------------
      Pixels above tolerance   : 0.00 %
  OK
```

7. If all goes well commit the .xmp and expected.png files



# How to add a new test (using specific driver)

1. Create a new directory

```
   <nnnn>-<meaningful name>
```

2. Create a file named test.sh into this directory

   This test.sh is a specific driver that can do whatever is necessary
   for the test. At the end the driver must return 0 if all is OK and
   1 otherwise.
