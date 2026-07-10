/*
 *    Copyright 2026 Ethan O'Connor
 *    This file is part of OpenOrienteering.
 *    SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENORIENTEERING_IMAGERY_SOURCE_RESOLVER_H
#define OPENORIENTEERING_IMAGERY_SOURCE_RESOLVER_H

#include "imagery_catalog.h"
#include "online_imagery_source.h"

namespace OpenOrienteering {

struct ImagerySourceResolveResult
{
	OnlineImagerySource source;
	QString error;
};

class ImagerySourceResolver
{
public:
	static ImagerySourceResolveResult resolve(const ImagerySourceDefinition& definition);
};

}  // namespace OpenOrienteering

#endif
