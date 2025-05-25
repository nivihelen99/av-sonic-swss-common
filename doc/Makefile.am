# Makefile.am for documentation files

# Add new markdown files here to ensure they are included in the distribution
# and potentially installed if an install rule for 'docdir' is defined.
# Using nobase_ to prevent prefixing with subdirectory name during install.
nobase_dist_doc_DATA = \
	redis_streams.md

# If you have other documentation files, list them here as well.
# For example:
# nobase_dist_doc_DATA += \
#   another_doc.md \
#   yet_another_doc.md

# If documentation needs to be installed to a specific location (e.g., $(datadir)/doc/$(PACKAGE_NAME))
# you would define 'docdir' and potentially install rules,
# but for now, just ensuring distribution is the primary goal.
# Example for installation (uncomment and adjust if needed):
# docdir = $(datadir)/doc/@PACKAGE_NAME@
# doc_DATA = $(nobase_dist_doc_DATA)

# Ensure clean-up if any generated files were part of documentation (not applicable for .md directly)
# CLEANFILES = 
# MAINTAINERCLEANFILES =
