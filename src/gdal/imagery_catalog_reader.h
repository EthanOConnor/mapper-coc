/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 */

#ifndef OPENORIENTEERING_IMAGERY_CATALOG_READER_H
#define OPENORIENTEERING_IMAGERY_CATALOG_READER_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include "imagery_catalog.h"

namespace OpenOrienteering {


struct ImageryCatalogIssue
{
	enum class Type {
		CatalogError,
		SourceError,
		UnsupportedSource,
	};

	Type type = Type::CatalogError;
	QString path;
	QString message;
	int source_index = -1;
};


struct ImageryCatalogReadResult
{
	ImageryCatalog catalog;
	QVector<ImageryCatalogIssue> issues;

	bool accepted() const;
	bool hasCatalogErrors() const;
};


class ImageryCatalogReader
{
public:
	static constexpr int max_document_size = 10 * 1024 * 1024;
	static constexpr int max_nesting_depth = 64;
	static constexpr int max_string_length = 16 * 1024;
	static constexpr int max_url_length = 8192;
	static constexpr int max_sources = 1000;
	static constexpr int max_tiles_per_source = 8;
	static constexpr int max_tile_matrices = 64;
	static constexpr int max_coverage_vertices = 10000;
	static constexpr int max_empty_status_codes = 32;

	static QString fileExtension();
	static ImageryCatalogReadResult read(const QByteArray& bytes);
	static TileMatrixSetDefinition webMercatorQuad();
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_IMAGERY_CATALOG_READER_H
