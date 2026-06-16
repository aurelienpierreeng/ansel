# Style Application Fixtures

`ansel-test-styles` reads this directory by default.

Each scenario is made of two required files and one optional file sharing the
same style basename:

```text
example.dtstyle
start_example.xmp  # optional
end_example.xmp
```

When `start_example.xmp` exists, the test loads it on a fresh copy of the testv image before applying `example.dtstyle`.<br>
When it is missing, the test applies the style on the base imported image history.<br>
The resulting pipe order is compared with the `module_order` loaded from `end_example.xmp`.<br>
The test also compares the final `enabled` state of each module instance against the last active history item loaded from the expected XMP.<br>
At the end of the run, the test prints one summary line per `.dtstyle` file:<br>
`PASSED` for matching fixtures, or `FAILED - <reason>` for the first detected failure in that scenario.
