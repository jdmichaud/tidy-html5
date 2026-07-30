man
===

This directory consists of file required for generating Tidy's documentation:

- `tidy1.xsl.in`, the stylesheet that turns the output of `tidy -xml-help` and
  `tidy -xml-config` into Tidy's `man` page. The prose sections are written
  here; the options are taken from Tidy's own help, so they never fall behind
  the source.

- `tidy.1`, a copy of that man page, generated from the release named in
  `version.txt`. The build generates a fresh one whenever `xsltproc` is
  available, and installs this copy when it is not, so that a man page is
  installed either way.

To bring `tidy.1` up to date, build with `xsltproc` installed and copy the
generated page over it:

    cmake --build . --target man
    cp tidy.1 ../man/tidy.1

Edit `tidy1.xsl.in` rather than `tidy.1`, or the next build will undo the
change. It is worth refreshing `tidy.1` when options are added or their
descriptions change, and before a release.

Please consult the main project README file for more information.
