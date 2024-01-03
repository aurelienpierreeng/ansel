
# Update from source code
intltool-update -p -g ansel

# Remove old translations
for f in *.po ; do
  msgattrib --translated --no-obsolete -o $f $f
done

# Report
intltool-update -g ansel -r
