First, make sure gcc-7.5.0 and g++-7.5.0 are installed.

If you want to use licensed FMOD or KDU build libraries (they are optional) you have to provision these yourself.
If you're licensing these with Phoenix/Firestorm, ask for the libraries for fmod and kdu. Put them into:
	/opt/firestorm

If you're a community builder, you'll need to build these libraries yourself, then change your autobuild.xml file to
point to your own versions, or create a different autobuild.xml with your customizations, and use this with autobuild
instead of our default autobuild.xml There are some examples of how to build FMOD on the LL Wiki and opensource-dev
mailing list. We've created a non-KDU build target to make this easier. Everywhere you see "ReleaseFS" below, use 
"ReleaseFS_open" instead.  This will perform the same build, using openjpeg instead of KDU.

Available premade firestorm-specific build targets:
	ReleaseFS (includes KDU, FMODSTUDIO)
	ReleaseFS_open (no KDU, no FMODSTUDIO)
	RelWithDebInfoFS_open (no KDU, no FMODSTUDIO)

To build firestorm:
	autobuild build -A64 -c ReleaseFS                        

Other examples:
	autobuild configure -A64 -c ReleaseFS  		                       # basic configuration step, don't build, just configure
	autobuild configure -A64 -c ReleaseFS -- --clean	               # clean the output area first, then configure
	autobuild configure -A64 -c ReleaseFS -- --chan Private-Yourname   # configure with a custom channel

	autobuild build -A64 -c ReleaseFS --no-configure		# default quick rebuild
	autobuild build -A64 -c ReleaseFS --no-configure -- --clean	# Clean rebuild
	autobuild build -A64 -c ReleaseFS -- --package		# Complete a build and package it into a tarball

When using the --package switch you can set the XZ_DEFAULTS variable to -T0 to use all available CPU cores
to create the .tar.xz file. This can significantly reduce the time needed to create the archive, but it will
use a lot more memory. For example:
	export XZ_DEFAULTS="-T0"
	autobuild build -A64 -c ReleaseFS_open -- --package

If you want to build with clang you can call autobuild like this:
   CC=clang CXX=clang++ autobuild configure -A64 -c ReleaseFS

Any of the configure options can also be used (and do the same thing) with the build options.
Typical LL autobuild configure options should also work, as long as they don't duplicate configuration we are
already doing.


Logs:
        Look for logs in build-linux-x86_64/logs

Output:
        Look for output in build-linux-x86_64/newview/Release
