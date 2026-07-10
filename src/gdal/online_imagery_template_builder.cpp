/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "online_imagery_template_builder.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QTextStream>
#include <QUrl>

#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map_coord.h"

namespace OpenOrienteering {

namespace {

constexpr double web_mercator_extent = 20037508.342789244;

double degToRad(double deg)
{
	return deg * M_PI / 180.0;
}

QString slugify(const QString& name)
{
	auto slug = name.toLower();
	slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
	slug.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
	if (slug.length() > 30)
		slug.truncate(30);
	return slug.isEmpty() ? QStringLiteral("online") : slug;
}

QString shortHash(const QString& input)
{
	auto hash = QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha1);
	return QString::fromLatin1(hash.toHex().left(6));
}

int matrixIndex(const OnlineImagerySource& source, const QString& id)
{
	for (int i = 0; i < source.tile_matrix_set.tile_matrices.size(); ++i)
		if (source.tile_matrix_set.tile_matrices.at(i).id == id)
			return i;
	return -1;
}

QString displayNameFromUrl(const QString& url)
{
	QUrl parsed(url);
	auto host = parsed.host();
	auto parts = host.split(QLatin1Char('.'));
	if (parts.size() >= 2)
		return parts[parts.size() - 2];
	return host.isEmpty() ? QStringLiteral("imagery") : host;
}

QString effectiveTemplateName(const OnlineImagerySource& source, const QString& template_name)
{
	auto const trimmed_name = template_name.trimmed();
	return trimmed_name.isEmpty() ? source.display_name : trimmed_name;
}

}  // namespace


OnlineImageryTemplateBuilder::ClassifyResult
OnlineImageryTemplateBuilder::classifyUrl(const QString& input)
{
	ClassifyResult result;
	auto trimmed = input.trimmed();

	if (trimmed.isEmpty())
	{
		result.error = tr("No URL provided.");
		return result;
	}

	if (trimmed.contains(QStringLiteral("{s}")) || trimmed.contains(QStringLiteral("${s}")))
	{
		result.error = tr("Subdomain placeholders ({s}) are not supported yet. Try a URL without {s}.");
		return result;
	}
	if (trimmed.contains(QStringLiteral("@2x")) || trimmed.contains(QStringLiteral("@3x")))
	{
		result.error = tr("HiDPI/retina tile URLs (@2x) are not supported yet.");
		return result;
	}

	if (trimmed.contains(QStringLiteral("/MapServer"), Qt::CaseInsensitive))
	{
		auto root = trimmed;
		auto tile_index = root.indexOf(QStringLiteral("/tile/"), 0, Qt::CaseInsensitive);
		if (tile_index >= 0)
			root.truncate(tile_index);

		if (!root.endsWith(QStringLiteral("/MapServer"), Qt::CaseInsensitive))
		{
			auto ms_index = root.lastIndexOf(QStringLiteral("/MapServer"), -1, Qt::CaseInsensitive);
			if (ms_index >= 0)
				root.truncate(ms_index + 10);
		}

		result.source.kind = OnlineImagerySource::Kind::ArcGisTiledMapServer;
		result.source.normalized_url = root;
		result.source.display_name = displayNameFromUrl(root);
		result.source.tile_size = QSize(256, 256);
		result.source.max_tile_level = 20;
		return result;
	}

	auto const has_z = trimmed.contains(QStringLiteral("{z}")) || trimmed.contains(QStringLiteral("${z}"));
	auto const has_x = trimmed.contains(QStringLiteral("{x}")) || trimmed.contains(QStringLiteral("${x}"));
	auto const has_y = trimmed.contains(QStringLiteral("{y}")) || trimmed.contains(QStringLiteral("${y}"));

	if (has_z && has_x && has_y)
	{
		auto normalized = trimmed;
		normalized.replace(QStringLiteral("{z}"), QStringLiteral("${z}"));
		normalized.replace(QStringLiteral("{x}"), QStringLiteral("${x}"));
		normalized.replace(QStringLiteral("{y}"), QStringLiteral("${y}"));

		result.source.kind = OnlineImagerySource::Kind::XyzTiles;
		result.source.normalized_url = normalized;
		result.source.display_name = displayNameFromUrl(trimmed);
		result.source.tile_size = QSize(256, 256);
		result.source.max_tile_level = 19;
		return result;
	}

	result.error = tr("Couldn't recognize this imagery link. Supported: top-origin Web Mercator XYZ tile URLs ({z}/{x}/{y}) and standard cached Web Mercator ArcGIS MapServer links.");
	return result;
}


OnlineImageryTemplateBuilder::GenerateResult
OnlineImageryTemplateBuilder::generateXml(
	const OnlineImagerySource& source,
	const QString& template_name,
	const QRectF& map_extent,
	const Georeferencing& georef,
	const QString& map_path)
{
	GenerateResult result;

	if (source.kind == OnlineImagerySource::Kind::Unknown)
	{
		result.error = tr("Unknown source type.");
		return result;
	}

	if (map_extent.isEmpty())
	{
		result.error = tr("No map coverage area was selected.");
		return result;
	}

	auto const generic_grid = !source.tile_matrix_set.tile_matrices.isEmpty();
	auto const generic_max_index = generic_grid ? matrixIndex(source, source.max_tile_matrix) : -1;
	auto const projection_tolerance = generic_max_index >= 0
	                                  ? source.tile_matrix_set.tile_matrices.at(generic_max_index).cell_size * 0.25
	                                  : 0.0;
	auto source_bbox = generic_grid
	                   ? mapExtentToSource(map_extent, georef, source.tile_matrix_set.crs, projection_tolerance)
	                   : mapExtentToWebMercator(map_extent, georef);
	if (source_bbox.width() <= 0 || source_bbox.height() <= 0)
	{
		result.error = tr("Could not convert the selected coverage area to the imagery source coordinates.");
		return result;
	}
	if (generic_grid && source.registration.operation_type == ImageryRegistration::OperationType::Translation2d)
		source_bbox.translate(-source.registration.dx, -source.registration.dy);

	auto tile_size = source.tile_size.width();
	if (tile_size <= 0)
		tile_size = 256;
	auto max_level = source.max_tile_level;
	if (max_level <= 0)
		max_level = 20;

	auto crop = generic_grid ? snapToTileGrid(source_bbox, source)
	                         : snapToTileGrid(source_bbox, max_level, tile_size);
	if (crop.pixel_width <= 0 || crop.pixel_height <= 0)
	{
		result.error = tr("The selected coverage does not intersect the imagery tile grid.");
		return result;
	}
	if (generic_grid && source.registration.operation_type == ImageryRegistration::OperationType::Translation2d)
	{
		crop.west += source.registration.dx;
		crop.east += source.registration.dx;
		crop.north += source.registration.dy;
		crop.south += source.registration.dy;
	}
	auto width_m = crop.east - crop.west;
	auto height_m = crop.north - crop.south;
	result.area_km2 = (width_m * height_m) / 1e6;

	QString server_url;
	switch (source.kind)
	{
	case OnlineImagerySource::Kind::XyzTiles:
		server_url = source.normalized_url;
		break;
	case OnlineImagerySource::Kind::ArcGisTiledMapServer:
		server_url = source.normalized_url + QStringLiteral("/tile/${z}/${y}/${x}");
		break;
	case OnlineImagerySource::Kind::Unknown:
		Q_UNREACHABLE();
	}
	if (generic_grid)
	{
		server_url.replace(QStringLiteral("{z}"), QStringLiteral("${z}"));
		server_url.replace(QStringLiteral("{x}"), QStringLiteral("${x}"));
		server_url.replace(QStringLiteral("{y}"), QStringLiteral("${y}"));
	}

	QString xml;
	QTextStream out(&xml);
	out << QStringLiteral("<GDAL_WMS>\n");
	if (!source.operational_fingerprint.isEmpty())
		out << QStringLiteral("  <!-- OperationalFingerprint: ") << QString::fromLatin1(source.operational_fingerprint) << QStringLiteral(" -->\n");
	out << QStringLiteral("  <Service name=\"TMS\">\n");
	out << QStringLiteral("    <ServerUrl>") << server_url.toHtmlEscaped() << QStringLiteral("</ServerUrl>\n");
	if (generic_grid && !source.format.isEmpty())
		out << QStringLiteral("    <ImageFormat>") << source.format.toHtmlEscaped() << QStringLiteral("</ImageFormat>\n");
	out << QStringLiteral("  </Service>\n");
	out << QStringLiteral("  <DataWindow>\n");
	out << QStringLiteral("    <UpperLeftX>") << QString::number(crop.west, 'f', 6) << QStringLiteral("</UpperLeftX>\n");
	out << QStringLiteral("    <UpperLeftY>") << QString::number(crop.north, 'f', 6) << QStringLiteral("</UpperLeftY>\n");
	out << QStringLiteral("    <LowerRightX>") << QString::number(crop.east, 'f', 6) << QStringLiteral("</LowerRightX>\n");
	out << QStringLiteral("    <LowerRightY>") << QString::number(crop.south, 'f', 6) << QStringLiteral("</LowerRightY>\n");
	out << QStringLiteral("    <SizeX>") << crop.pixel_width << QStringLiteral("</SizeX>\n");
	out << QStringLiteral("    <SizeY>") << crop.pixel_height << QStringLiteral("</SizeY>\n");
	out << QStringLiteral("    <TileLevel>") << crop.tile_level << QStringLiteral("</TileLevel>\n");
	if (generic_grid)
	{
		auto const min_index = matrixIndex(source, source.min_tile_matrix);
		auto const max_index = matrixIndex(source, source.max_tile_matrix);
		out << QStringLiteral("    <OverviewCount>") << max_index - min_index << QStringLiteral("</OverviewCount>\n");
	}
	out << QStringLiteral("    <TileX>") << crop.tile_x_min << QStringLiteral("</TileX>\n");
	out << QStringLiteral("    <TileY>") << crop.tile_y_min << QStringLiteral("</TileY>\n");
	out << QStringLiteral("    <YOrigin>") << (source.scheme == QLatin1String("tms") ? QStringLiteral("bottom") : QStringLiteral("top")) << QStringLiteral("</YOrigin>\n");
	out << QStringLiteral("  </DataWindow>\n");
	out << QStringLiteral("  <Projection>") << (generic_grid ? source.tile_matrix_set.crs.toHtmlEscaped() : QStringLiteral("EPSG:3857")) << QStringLiteral("</Projection>\n");
	out << QStringLiteral("  <BlockSizeX>") << tile_size << QStringLiteral("</BlockSizeX>\n");
	out << QStringLiteral("  <BlockSizeY>") << tile_size << QStringLiteral("</BlockSizeY>\n");
	out << QStringLiteral("  <BandsCount>3</BandsCount>\n");
	QStringList zero_block_http_codes;
	if (generic_grid && !source.request.empty_http_status_codes.isEmpty())
	{
		for (auto code : source.request.empty_http_status_codes)
			zero_block_http_codes.push_back(QString::number(code));
	}
	else
		zero_block_http_codes.push_back(QStringLiteral("404"));
	out << QStringLiteral("  <ZeroBlockHttpCodes>") << zero_block_http_codes.join(QLatin1Char(',')) << QStringLiteral("</ZeroBlockHttpCodes>\n");
	if (generic_grid && !source.request.referer.isEmpty())
		out << QStringLiteral("  <Referer>") << source.request.referer.toHtmlEscaped() << QStringLiteral("</Referer>\n");
	// Keep GDAL's default per-user cache location instead of baking a
	// machine-local absolute cache path into the generated template file.
	out << QStringLiteral("  <Cache />\n");
	out << QStringLiteral("</GDAL_WMS>\n");

	auto output_path = outputFileName(map_path, source, template_name);
	QFile file(output_path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		result.error = tr("Could not write imagery file: %1").arg(file.errorString());
		return result;
	}
	if (file.write(xml.toUtf8()) < 0)
	{
		result.error = tr("Could not write imagery file: %1").arg(file.errorString());
		return result;
	}

	result.xml_path = output_path;
	return result;
}


QString OnlineImageryTemplateBuilder::outputFileName(
	const QString& map_path,
	const OnlineImagerySource& source,
	const QString& template_name)
{
	QFileInfo map_info(map_path);
	auto map_basename = map_info.completeBaseName();
	auto slug = slugify(effectiveTemplateName(source, template_name));
	auto hash = source.operational_fingerprint.isEmpty()
	            ? shortHash(source.normalized_url)
	            : QString::fromLatin1(source.operational_fingerprint.left(12));
	auto filename = QStringLiteral("%1_%2_online_%3.xml").arg(slug, map_basename, hash);
	auto path = map_info.absoluteDir().filePath(filename);
	if (source.operational_fingerprint.isEmpty() || !QFileInfo::exists(path))
		return path;
	QFile existing(path);
	if (existing.open(QIODevice::ReadOnly) && existing.readAll().contains(source.operational_fingerprint))
		return path;
	for (int length = 16; length <= source.operational_fingerprint.size(); length += 4)
	{
		filename = QStringLiteral("%1_%2_online_%3.xml").arg(slug, map_basename, QString::fromLatin1(source.operational_fingerprint.left(length)));
		path = map_info.absoluteDir().filePath(filename);
		if (!QFileInfo::exists(path))
			return path;
	}
	return {};
}


QPointF OnlineImageryTemplateBuilder::latLonToWebMercator(double lat_deg, double lon_deg)
{
	auto x = lon_deg * web_mercator_extent / 180.0;
	auto lat_rad = degToRad(lat_deg);
	auto y = std::log(std::tan(M_PI / 4.0 + lat_rad / 2.0)) * web_mercator_extent / M_PI;
	return QPointF(x, y);
}


QRectF OnlineImageryTemplateBuilder::mapExtentToWebMercator(
	const QRectF& map_extent,
	const Georeferencing& georef)
{
	if (georef.getState() != Georeferencing::Geospatial)
		return {};

	bool ok = false;
	auto tl = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(map_extent.topLeft())), &ok);
	if (!ok)
		return {};
	auto tr = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(map_extent.topRight())), &ok);
	if (!ok)
		return {};
	auto bl = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(map_extent.bottomLeft())), &ok);
	if (!ok)
		return {};
	auto br = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(map_extent.bottomRight())), &ok);
	if (!ok)
		return {};

	auto m_tl = latLonToWebMercator(tl.latitude(), tl.longitude());
	auto m_tr = latLonToWebMercator(tr.latitude(), tr.longitude());
	auto m_bl = latLonToWebMercator(bl.latitude(), bl.longitude());
	auto m_br = latLonToWebMercator(br.latitude(), br.longitude());

	auto min_x = std::min({m_tl.x(), m_tr.x(), m_bl.x(), m_br.x()});
	auto max_x = std::max({m_tl.x(), m_tr.x(), m_bl.x(), m_br.x()});
	auto min_y = std::min({m_tl.y(), m_tr.y(), m_bl.y(), m_br.y()});
	auto max_y = std::max({m_tl.y(), m_tr.y(), m_bl.y(), m_br.y()});
	return QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
}


QRectF OnlineImageryTemplateBuilder::mapExtentToSource(
	const QRectF& map_extent,
	const Georeferencing& georef,
	const QString& crs,
	double tolerance)
{
	if (georef.getState() != Georeferencing::Geospatial)
		return {};
	auto transform = ProjTransform(crs);
	if (!transform.isValid())
		return {};
	auto project = [&georef, &transform](const QPointF& point, QPointF* output) {
		bool ok = false;
		auto const geographic = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(point)), &ok);
		if (!ok)
			return false;
		*output = transform.forward(geographic, &ok);
		return ok && std::isfinite(output->x()) && std::isfinite(output->y());
	};
	QVector<QPointF> projected;
	int samples = 0;
	std::function<bool(const QPointF&, const QPointF&, const QPointF&, const QPointF&, int)> subdivide;
	subdivide = [&](const QPointF& map_a, const QPointF& map_b, const QPointF& source_a, const QPointF& source_b, int depth) {
		if (++samples > 4096)
			return false;
		auto const map_mid = (map_a + map_b) / 2.0;
		QPointF source_mid;
		if (!project(map_mid, &source_mid))
			return false;
		auto const chord_mid = (source_a + source_b) / 2.0;
		auto const deviation = std::hypot(source_mid.x() - chord_mid.x(), source_mid.y() - chord_mid.y());
		if (deviation <= tolerance)
		{
			projected.push_back(source_mid);
			return true;
		}
		if (depth >= 12)
			return false;
		return subdivide(map_a, map_mid, source_a, source_mid, depth + 1)
		       && subdivide(map_mid, map_b, source_mid, source_b, depth + 1);
	};
	auto const corners = QVector<QPointF> { map_extent.topLeft(), map_extent.topRight(), map_extent.bottomRight(), map_extent.bottomLeft() };
	QVector<QPointF> source_corners;
	for (auto const& point : corners)
	{
		QPointF source_point;
		if (!project(point, &source_point))
			return {};
		source_corners.push_back(source_point);
		projected.push_back(source_point);
	}
	for (int i = 0; i < corners.size(); ++i)
		if (!subdivide(corners.at(i), corners.at((i + 1) % corners.size()), source_corners.at(i), source_corners.at((i + 1) % corners.size()), 0))
			return {};
	auto min_x = projected.first().x();
	auto max_x = min_x;
	auto min_y = projected.first().y();
	auto max_y = min_y;
	for (auto const& point : projected)
	{
		min_x = std::min(min_x, point.x()); max_x = std::max(max_x, point.x());
		min_y = std::min(min_y, point.y()); max_y = std::max(max_y, point.y());
	}
	return QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
}


OnlineImageryTemplateBuilder::TileGridCrop
OnlineImageryTemplateBuilder::snapToTileGrid(
	const QRectF& mercator_bbox,
	int max_tile_level,
	int tile_size)
{
	TileGridCrop crop;
	crop.tile_level = max_tile_level;

	auto world_size = 2.0 * web_mercator_extent;
	auto num_tiles = std::pow(2.0, max_tile_level);
	auto tile_span = world_size / num_tiles;
	auto origin_x = -web_mercator_extent;
	auto origin_y = web_mercator_extent;

	auto merc_south = mercator_bbox.top();
	auto merc_north = mercator_bbox.bottom();
	crop.tile_x_min = int(std::floor((mercator_bbox.left() - origin_x) / tile_span));
	crop.tile_x_max = int(std::ceil((mercator_bbox.right() - origin_x) / tile_span)) - 1;
	crop.tile_y_min = int(std::floor((origin_y - merc_north) / tile_span));
	crop.tile_y_max = int(std::ceil((origin_y - merc_south) / tile_span)) - 1;

	auto max_tile_idx = int(num_tiles) - 1;
	crop.tile_x_min = std::max(0, crop.tile_x_min);
	crop.tile_y_min = std::max(0, crop.tile_y_min);
	crop.tile_x_max = std::min(max_tile_idx, crop.tile_x_max);
	crop.tile_y_max = std::min(max_tile_idx, crop.tile_y_max);

	// Pad the cropped origin to a power-of-two tile boundary so GDAL's
	// WMS/TMS overview requests keep the source registration aligned.
	// Extra padded tiles outside the intended coverage are harmless and
	// typically return 404s, which the generated XML already treats as
	// zero blocks.
	constexpr int align_bits = 5;  // 32-tile boundary
	constexpr int align_mask = (1 << align_bits) - 1;
	crop.tile_x_min &= ~align_mask;
	crop.tile_y_min &= ~align_mask;
	crop.tile_x_max |= align_mask;
	crop.tile_y_max |= align_mask;
	crop.tile_x_max = std::min(crop.tile_x_max, max_tile_idx);
	crop.tile_y_max = std::min(crop.tile_y_max, max_tile_idx);

	crop.west = origin_x + crop.tile_x_min * tile_span;
	crop.north = origin_y - crop.tile_y_min * tile_span;
	crop.east = origin_x + (crop.tile_x_max + 1) * tile_span;
	crop.south = origin_y - (crop.tile_y_max + 1) * tile_span;
	crop.pixel_width = (crop.tile_x_max - crop.tile_x_min + 1) * tile_size;
	crop.pixel_height = (crop.tile_y_max - crop.tile_y_min + 1) * tile_size;
	return crop;
}


OnlineImageryTemplateBuilder::TileGridCrop
OnlineImageryTemplateBuilder::snapToTileGrid(const QRectF& source_bbox, const OnlineImagerySource& source)
{
	TileGridCrop crop;
	auto const min_index = matrixIndex(source, source.min_tile_matrix);
	auto const max_index = matrixIndex(source, source.max_tile_matrix);
	if (min_index < 0 || max_index < min_index)
		return crop;
	auto const& matrix = source.tile_matrix_set.tile_matrices.at(max_index);
	bool level_ok = false;
	crop.tile_level = matrix.id.toInt(&level_ok);
	if (!level_ok)
		return {};
	auto const span_x = matrix.cell_size * matrix.tile_size.width();
	auto const span_y = matrix.cell_size * matrix.tile_size.height();
	auto min_col = qint64(0), max_col = matrix.matrix_width - 1;
	auto min_row = qint64(0), max_row = matrix.matrix_height - 1;
	for (auto const& limit : source.tile_matrix_limits)
	{
		if (limit.tile_matrix == matrix.id)
		{
			min_col = limit.min_tile_col; max_col = limit.max_tile_col;
			min_row = limit.min_tile_row; max_row = limit.max_tile_row;
		}
	}
	auto const top_origin = matrix.corner_of_origin == QLatin1String("topLeft");
	auto const bbox_south = source_bbox.top();
	auto const bbox_north = source_bbox.bottom();
	auto x_min = qint64(std::floor((source_bbox.left() - matrix.point_of_origin.x()) / span_x));
	auto x_max = qint64(std::ceil((source_bbox.right() - matrix.point_of_origin.x()) / span_x)) - 1;
	auto y_min = top_origin
	             ? qint64(std::floor((matrix.point_of_origin.y() - bbox_north) / span_y))
	             : qint64(std::floor((bbox_south - matrix.point_of_origin.y()) / span_y));
	auto y_max = top_origin
	             ? qint64(std::ceil((matrix.point_of_origin.y() - bbox_south) / span_y)) - 1
	             : qint64(std::ceil((bbox_north - matrix.point_of_origin.y()) / span_y)) - 1;
	x_min = std::max({ x_min, min_col, qint64(0) });
	x_max = std::min({ x_max, max_col, matrix.matrix_width - 1 });
	y_min = std::max({ y_min, min_row, qint64(0) });
	y_max = std::min({ y_max, max_row, matrix.matrix_height - 1 });
	if (x_min > x_max || y_min > y_max)
		return {};

	// Keep the cropped origin aligned with GDAL's power-of-two TMS overviews.
	// Padding may cross tile matrix limits: missing tiles are transparent, and
	// preserving overview alignment is more important than a tight crop there.
	constexpr qint64 origin_alignment = 64;
	x_min -= x_min % origin_alignment;
	y_min -= y_min % origin_alignment;
	crop.tile_x_min = int(x_min); crop.tile_x_max = int(x_max);
	crop.tile_y_min = int(y_min); crop.tile_y_max = int(y_max);
	crop.west = matrix.point_of_origin.x() + x_min * span_x;
	crop.east = matrix.point_of_origin.x() + (x_max + 1) * span_x;
	if (top_origin)
	{
		crop.north = matrix.point_of_origin.y() - y_min * span_y;
		crop.south = matrix.point_of_origin.y() - (y_max + 1) * span_y;
	}
	else
	{
		crop.south = matrix.point_of_origin.y() + y_min * span_y;
		crop.north = matrix.point_of_origin.y() + (y_max + 1) * span_y;
	}
	crop.pixel_width = int((x_max - x_min + 1) * matrix.tile_size.width());
	crop.pixel_height = int((y_max - y_min + 1) * matrix.tile_size.height());
	return crop;
}


}  // namespace OpenOrienteering
