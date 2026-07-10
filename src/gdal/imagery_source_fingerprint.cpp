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

#include "imagery_source_fingerprint.h"

#include <algorithm>
#include <limits>

#include <QJsonArray>
#include <QSet>
#include <QUrl>

#include "imagery_json_canonicalizer.h"

namespace OpenOrienteering {

namespace {

QString normalizeUrl(QString url)
{
	url.replace(QStringLiteral("${z}"), QStringLiteral("{z}"));
	url.replace(QStringLiteral("${x}"), QStringLiteral("{x}"));
	url.replace(QStringLiteral("${y}"), QStringLiteral("{y}"));

	auto const scheme_end = url.indexOf(QStringLiteral("://"));
	if (scheme_end < 0)
		return url;
	auto const authority_start = scheme_end + 3;
	auto authority_end = url.size();
	for (auto const separator : { QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#') })
	{
		auto const position = url.indexOf(separator, authority_start);
		if (position >= 0)
			authority_end = std::min(authority_end, position);
	}

	auto probe = url;
	probe.replace(QStringLiteral("{z}"), QStringLiteral("0"));
	probe.replace(QStringLiteral("{x}"), QStringLiteral("0"));
	probe.replace(QStringLiteral("{y}"), QStringLiteral("0"));
	auto const parsed = QUrl(probe, QUrl::StrictMode);
	auto host = QString::fromLatin1(QUrl::toAce(parsed.host())).toLower();
	if (host.contains(QLatin1Char(':')))
		host = QLatin1Char('[') + host + QLatin1Char(']');
	auto const scheme = url.left(scheme_end).toLower();
	auto const port = parsed.port(-1);
	auto const is_default_port = (scheme == QLatin1String("http") && port == 80)
	                             || (scheme == QLatin1String("https") && port == 443);
	QString authority = host;
	if (port >= 0 && !is_default_port)
		authority += QLatin1Char(':') + QString::number(port);
	return scheme + QStringLiteral("://") + authority + url.mid(authority_end);
}


QJsonArray sortedStrings(const QStringList& values)
{
	auto sorted = values;
	sorted.removeDuplicates();
	std::sort(sorted.begin(), sorted.end());
	QJsonArray array;
	for (auto const& value : sorted)
		array.push_back(value);
	return array;
}


QJsonArray sortedIntegers(const QVector<int>& values)
{
	auto sorted = values;
	std::sort(sorted.begin(), sorted.end());
	sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
	QJsonArray array;
	for (auto value : sorted)
		array.push_back(value);
	return array;
}


QJsonObject normalizeRequest(const ImagerySourceDefinition& source)
{
	QJsonObject request;
	if (!source.request.referer.isEmpty())
		request.insert(QStringLiteral("referer"), normalizeUrl(source.request.referer));
	if (!source.request.empty_http_status_codes.isEmpty())
		request.insert(QStringLiteral("emptyHttpStatusCodes"), sortedIntegers(source.request.empty_http_status_codes));
	return request;
}


QJsonObject operationalMatrixSet(const ImagerySourceDefinition& source)
{
	auto const& definition = source.tile_matrix_set;
	if (definition.tile_matrices.isEmpty())
		return {};
	QJsonArray matrices;
	auto const original_matrices = definition.original_object.value(QStringLiteral("tileMatrices")).toArray();
	for (int i = 0; i < definition.tile_matrices.size(); ++i)
	{
		auto const& matrix = definition.tile_matrices.at(i);
		QJsonObject object {
			{ QStringLiteral("id"), matrix.id },
			{ QStringLiteral("scaleDenominator"), matrix.scale_denominator },
			{ QStringLiteral("cellSize"), matrix.cell_size },
			{ QStringLiteral("pointOfOrigin"), QJsonArray { matrix.point_of_origin.x(), matrix.point_of_origin.y() } },
			{ QStringLiteral("cornerOfOrigin"), matrix.corner_of_origin },
			{ QStringLiteral("tileWidth"), matrix.tile_size.width() },
			{ QStringLiteral("tileHeight"), matrix.tile_size.height() },
			{ QStringLiteral("matrixWidth"), double(matrix.matrix_width) },
			{ QStringLiteral("matrixHeight"), double(matrix.matrix_height) }
		};
		if (matrix.has_variable_matrix_widths && i < original_matrices.size())
			object.insert(QStringLiteral("variableMatrixWidths"), original_matrices.at(i).toObject().value(QStringLiteral("variableMatrixWidths")));
		matrices.push_back(object);
	}
	QJsonObject matrix_set {
		{ QStringLiteral("id"), definition.id },
		{ QStringLiteral("crs"), definition.crs },
		{ QStringLiteral("tileMatrices"), matrices }
	};
	if (!definition.ordered_axes.isEmpty())
		matrix_set.insert(QStringLiteral("orderedAxes"), QJsonArray::fromStringList(definition.ordered_axes));
	return matrix_set;
}


QJsonArray normalizedLimits(const ImagerySourceDefinition& source)
{
	QVector<TileMatrixLimitsDefinition> limits = source.tile_matrix_limits;
	auto matrix_index = [&source](const QString& id) {
		for (int i = 0; i < source.tile_matrix_set.tile_matrices.size(); ++i)
			if (source.tile_matrix_set.tile_matrices.at(i).id == id)
				return i;
		return std::numeric_limits<int>::max();
	};
	std::sort(limits.begin(), limits.end(), [&matrix_index](const TileMatrixLimitsDefinition& first, const TileMatrixLimitsDefinition& second) {
		auto const first_index = matrix_index(first.tile_matrix);
		auto const second_index = matrix_index(second.tile_matrix);
		if (first_index != second_index) return first_index < second_index;
		if (first.min_tile_row != second.min_tile_row) return first.min_tile_row < second.min_tile_row;
		if (first.max_tile_row != second.max_tile_row) return first.max_tile_row < second.max_tile_row;
		if (first.min_tile_col != second.min_tile_col) return first.min_tile_col < second.min_tile_col;
		return first.max_tile_col < second.max_tile_col;
	});
	QJsonArray array;
	for (auto const& limit : limits)
	{
		array.push_back(QJsonObject {
			{ QStringLiteral("tileMatrix"), limit.tile_matrix },
			{ QStringLiteral("minTileRow"), double(limit.min_tile_row) },
			{ QStringLiteral("maxTileRow"), double(limit.max_tile_row) },
			{ QStringLiteral("minTileCol"), double(limit.min_tile_col) },
			{ QStringLiteral("maxTileCol"), double(limit.max_tile_col) }
		});
	}
	return array;
}


QJsonObject normalizedRegistration(const ImagerySourceDefinition& source)
{
	if (source.registration.operation_type == ImageryRegistration::OperationType::None)
		return {};
	auto registration = source.registration.original_object;
	auto source_frame = registration.value(QStringLiteral("sourceFrame")).toObject();
	source_frame.insert(QStringLiteral("crs"), source.registration.source_crs);
	registration.insert(QStringLiteral("sourceFrame"), source_frame);
	auto target_frame = registration.value(QStringLiteral("targetFrame")).toObject();
	target_frame.insert(QStringLiteral("crs"), source.registration.target_crs);
	registration.insert(QStringLiteral("targetFrame"), target_frame);
	return registration;
}


QStringList operationalCapabilities(const ImagerySourceDefinition& source)
{
	auto capabilities = source.required_capabilities;
	capabilities.push_back(QStringLiteral("tile-matrix-set.ogc-2.0"));
	capabilities.push_back(source.tile_matrix_set.is_dyadic
	                         ? QStringLiteral("tile-matrix-set.dyadic.v1")
	                         : QStringLiteral("tile-matrix-set.nondyadic.v1"));
	switch (source.registration.operation_type)
	{
	case ImageryRegistration::OperationType::Translation2d:
		capabilities.push_back(QStringLiteral("registration.translation2d.v1"));
		break;
	case ImageryRegistration::OperationType::Affine2d:
		capabilities.push_back(QStringLiteral("registration.affine2d.v1"));
		break;
	case ImageryRegistration::OperationType::GridShift:
		capabilities.push_back(QStringLiteral("registration.grid-shift.v1"));
		break;
	case ImageryRegistration::OperationType::None:
		break;
	}
	return capabilities;
}


void normalizeNoticeUrls(QJsonObject& source)
{
	if (!source.value(QStringLiteral("notices")).isObject())
		return;
	auto notices = source.value(QStringLiteral("notices")).toObject();
	for (auto const& name : { QStringLiteral("attributionUrl"), QStringLiteral("sourceUrl"), QStringLiteral("termsUrl"), QStringLiteral("privacyUrl") })
	{
		if (notices.value(name).isString())
			notices.insert(name, normalizeUrl(notices.value(name).toString()));
	}
	source.insert(QStringLiteral("notices"), notices);
}


QJsonObject normalizedFullSource(const ImagerySourceDefinition& source)
{
	auto normalized = source.original_object;
	QJsonArray tiles;
	for (auto const& tile : source.tiles)
		tiles.push_back(normalizeUrl(tile));
	normalized.insert(QStringLiteral("tiles"), tiles);
	normalized.insert(QStringLiteral("format"), source.format);
	normalized.insert(QStringLiteral("minTileMatrix"), source.min_tile_matrix);
	normalized.insert(QStringLiteral("maxTileMatrix"), source.max_tile_matrix);
	if (!source.tile_matrix_set.tile_matrices.isEmpty())
	{
		normalized.remove(QStringLiteral("tileMatrixSetURI"));
		auto matrix_set = source.tile_matrix_set.original_object;
		matrix_set.insert(QStringLiteral("crs"), source.tile_matrix_set.crs);
		normalized.insert(QStringLiteral("tileMatrixSet"), matrix_set);
	}
	else if (!source.tile_matrix_set_uri.isEmpty())
	{
		normalized.insert(QStringLiteral("tileMatrixSetURI"), normalizeUrl(source.tile_matrix_set_uri));
	}
	if (normalized.contains(QStringLiteral("requires")))
		normalized.insert(QStringLiteral("requires"), sortedStrings(source.required_capabilities));
	if (normalized.contains(QStringLiteral("request")))
		normalized.insert(QStringLiteral("request"), normalizeRequest(source));
	if (normalized.contains(QStringLiteral("tileMatrixLimits")))
		normalized.insert(QStringLiteral("tileMatrixLimits"), normalizedLimits(source));
	if (normalized.contains(QStringLiteral("registration")))
		normalized.insert(QStringLiteral("registration"), normalizedRegistration(source));
	normalizeNoticeUrls(normalized);
	return normalized;
}


QJsonObject normalizedOperationalSource(const ImagerySourceDefinition& source)
{
	QJsonArray tiles;
	for (auto const& tile : source.tiles)
		tiles.push_back(normalizeUrl(tile));
	QJsonObject normalized {
		{ QStringLiteral("fingerprintVersion"), ImagerySourceFingerprints::version },
		{ QStringLiteral("type"), source.type },
		{ QStringLiteral("tiles"), tiles },
		{ QStringLiteral("scheme"), source.scheme },
		{ QStringLiteral("format"), source.format },
		{ QStringLiteral("minTileMatrix"), source.min_tile_matrix },
		{ QStringLiteral("maxTileMatrix"), source.max_tile_matrix },
		{ QStringLiteral("tileMatrixLimits"), normalizedLimits(source) },
		{ QStringLiteral("request"), normalizeRequest(source) },
		{ QStringLiteral("registration"), source.registration.operation_type == ImageryRegistration::OperationType::None
		                                     ? QJsonValue(QJsonValue::Null)
		                                     : QJsonValue(normalizedRegistration(source)) },
		{ QStringLiteral("requires"), sortedStrings(operationalCapabilities(source)) }
	};
	if (!source.tile_matrix_set.tile_matrices.isEmpty())
		normalized.insert(QStringLiteral("tileMatrixSet"), operationalMatrixSet(source));
	else
		normalized.insert(QStringLiteral("tileMatrixSetURI"), normalizeUrl(source.tile_matrix_set_uri));
	return normalized;
}

}  // namespace


bool ImagerySourceFingerprint::calculate(const ImagerySourceDefinition& source,
	                                      ImagerySourceFingerprints* fingerprints,
	                                      QString* error)
{
	if (!fingerprints)
	{
		if (error)
			*error = QStringLiteral("Fingerprint output pointer is null");
		return false;
	}
	fingerprints->normalized_full = QJsonObject {
		{ QStringLiteral("fingerprintVersion"), ImagerySourceFingerprints::version },
		{ QStringLiteral("source"), normalizedFullSource(source) }
	};
	fingerprints->normalized_operational = normalizedOperationalSource(source);
	QByteArray full_canonical;
	QByteArray operational_canonical;
	if (!ImageryJsonCanonicalizer::canonicalize(fingerprints->normalized_full, &full_canonical, error)
	    || !ImageryJsonCanonicalizer::canonicalize(fingerprints->normalized_operational, &operational_canonical, error))
	{
		fingerprints->full.clear();
		fingerprints->operational.clear();
		return false;
	}
	fingerprints->full = ImageryJsonCanonicalizer::sha256(full_canonical);
	fingerprints->operational = ImageryJsonCanonicalizer::sha256(operational_canonical);
	if (error)
		error->clear();
	return true;
}


}  // namespace OpenOrienteering
