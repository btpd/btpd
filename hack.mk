clean:
	find . -name \*~ -print0 | xargs -0 rm
	find . -name \*.in -print0 | xargs -0 rm
	rm -rf aclocal.m4 autom4te.cache compile config.* configure depcomp install-sh missing btpd-*.tar.gz
