#
#    Copyright 2026 Ethan O'Connor
#
#    This file is part of OpenOrienteering.
#
#    OpenOrienteering is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#

if(NOT LICENSING_RUNTIME_NOTICES OR NOT EXISTS "${LICENSING_RUNTIME_NOTICES}")
	message(FATAL_ERROR
	  "LICENSING_RUNTIME_NOTICES must name an existing aggregate notice file")
endif()

# The runtime-bundle notice is derived from the exact packaged DLL closure.
# Retain notices for components which are built from this source tree.
set(third_party_components runtime_bundle qtsingleapplication)
if(Mapper_BUILD_CLIPPER)
	list(APPEND third_party_components libpolyclipping)
endif()
if(TARGET cove)
	list(APPEND third_party_components potrace)
endif()

set(explicit_copyright_runtime_bundle
  "runtime-bundle.txt"
  "${LICENSING_RUNTIME_NOTICES}"
  "3rd-party"
)
