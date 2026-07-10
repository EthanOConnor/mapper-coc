/*
 *    Copyright 2026 Ethan O'Connor
 *    This file is part of OpenOrienteering.
 *    SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "imagery_source_resolver.h"

namespace OpenOrienteering {

ImagerySourceResolveResult ImagerySourceResolver::resolve(const ImagerySourceDefinition& definition)
{
	ImagerySourceResolveResult result;
	if (!definition.valid)
	{
		result.error = QStringLiteral("Invalid imagery source definition");
		return result;
	}
	if (!definition.supported)
	{
		result.error = QStringLiteral("Unsupported imagery source capabilities: %1")
		               .arg(definition.unsupported_capabilities.join(QStringLiteral(", ")));
		return result;
	}
	if (definition.tiles.isEmpty() || definition.tile_matrix_set.tile_matrices.isEmpty())
	{
		result.error = QStringLiteral("Imagery source is missing a resolved tile grid");
		return result;
	}
	result.source.kind = OnlineImagerySource::Kind::XyzTiles;
	result.source.display_name = definition.name;
	result.source.normalized_url = definition.tiles.first();
	result.source.scheme = definition.scheme;
	result.source.format = definition.format;
	result.source.min_tile_matrix = definition.min_tile_matrix;
	result.source.max_tile_matrix = definition.max_tile_matrix;
	result.source.tile_matrix_set = definition.tile_matrix_set;
	result.source.tile_matrix_limits = definition.tile_matrix_limits;
	result.source.request = definition.request;
	result.source.registration = definition.registration;
	result.source.operational_fingerprint = definition.operational_fingerprint;
	auto const& matrices = definition.tile_matrix_set.tile_matrices;
	result.source.tile_size = matrices.last().tile_size;
	result.source.max_tile_level = matrices.last().id.toInt();
	return result;
}

}  // namespace OpenOrienteering
