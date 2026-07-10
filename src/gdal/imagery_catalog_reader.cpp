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

#include "imagery_catalog_reader.h"

#include <cmath>
#include <limits>

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <proj.h>

#include "imagery_json_canonicalizer.h"
#include "imagery_source_fingerprint.h"

namespace OpenOrienteering {

namespace {

constexpr auto catalog_format = "org.openorienteering.imagery-catalog";
constexpr auto web_mercator_quad_uri = "http://www.opengis.net/def/tilematrixset/OGC/1.0/WebMercatorQuad";
constexpr double max_exact_integer = 9007199254740991.0;


bool isValidUtf8(const QByteArray& bytes)
{
	auto const* data = reinterpret_cast<const unsigned char*>(bytes.constData());
	auto const size = bytes.size();
	for (int i = 0; i < size; ++i)
	{
		auto const first = data[i];
		if (first <= 0x7f)
			continue;

		int continuation_count = 0;
		uint codepoint = 0;
		if (first >= 0xc2 && first <= 0xdf)
		{
			continuation_count = 1;
			codepoint = first & 0x1f;
		}
		else if (first >= 0xe0 && first <= 0xef)
		{
			continuation_count = 2;
			codepoint = first & 0x0f;
		}
		else if (first >= 0xf0 && first <= 0xf4)
		{
			continuation_count = 3;
			codepoint = first & 0x07;
		}
		else
		{
			return false;
		}

		if (i + continuation_count >= size)
			return false;
		for (int j = 0; j < continuation_count; ++j)
		{
			auto const next = data[++i];
			if ((next & 0xc0) != 0x80)
				return false;
			codepoint = (codepoint << 6) | (next & 0x3f);
		}

		if ((continuation_count == 2 && codepoint < 0x800)
		    || (continuation_count == 3 && codepoint < 0x10000)
		    || codepoint > 0x10ffff
		    || (codepoint >= 0xd800 && codepoint <= 0xdfff))
		{
			return false;
		}
	}
	return true;
}


class JsonPreflight
{
public:
	explicit JsonPreflight(const QByteArray& input)
	: input(input)
	{}

	bool validate()
	{
		if (!isValidUtf8(input))
			return fail(QStringLiteral("Catalog is not valid UTF-8"));
		skipSpace();
		if (!parseValue(1))
			return false;
		skipSpace();
		if (position != input.size())
			return fail(QStringLiteral("Unexpected data after the JSON document"));
		return true;
	}

	QString errorString() const { return error; }

private:
	bool parseValue(int depth)
	{
		if (depth > ImageryCatalogReader::max_nesting_depth)
			return fail(QStringLiteral("JSON nesting exceeds %1 levels").arg(ImageryCatalogReader::max_nesting_depth));
		if (position >= input.size())
			return fail(QStringLiteral("Unexpected end of JSON input"));

		switch (input.at(position))
		{
		case '{': return parseObject(depth);
		case '[': return parseArray(depth);
		case '"': return parseString(nullptr);
		case 't': return parseLiteral("true");
		case 'f': return parseLiteral("false");
		case 'n': return parseLiteral("null");
		default: return parseNumber();
		}
	}

	bool parseObject(int depth)
	{
		++position;
		skipSpace();
		QSet<QString> keys;
		if (consume('}'))
			return true;
		while (position < input.size())
		{
			QString key;
			if (!parseString(&key))
				return false;
			if (keys.contains(key))
				return fail(QStringLiteral("Duplicate JSON object member: %1").arg(key));
			keys.insert(key);
			skipSpace();
			if (!consume(':'))
				return fail(QStringLiteral("Expected ':' after an object member name"));
			skipSpace();
			if (!parseValue(depth + 1))
				return false;
			skipSpace();
			if (consume('}'))
				return true;
			if (!consume(','))
				return fail(QStringLiteral("Expected ',' or '}' in an object"));
			skipSpace();
		}
		return fail(QStringLiteral("Unterminated JSON object"));
	}

	bool parseArray(int depth)
	{
		++position;
		skipSpace();
		if (consume(']'))
			return true;
		while (position < input.size())
		{
			if (!parseValue(depth + 1))
				return false;
			skipSpace();
			if (consume(']'))
				return true;
			if (!consume(','))
				return fail(QStringLiteral("Expected ',' or ']' in an array"));
			skipSpace();
		}
		return fail(QStringLiteral("Unterminated JSON array"));
	}

	bool parseString(QString* output)
	{
		if (!consume('"'))
			return fail(QStringLiteral("Expected a JSON string"));
		QString decoded;
		while (position < input.size())
		{
			auto const byte = static_cast<unsigned char>(input.at(position++));
			if (byte == '"')
			{
				if (decoded.size() > ImageryCatalogReader::max_string_length)
					return fail(QStringLiteral("JSON string exceeds %1 characters").arg(ImageryCatalogReader::max_string_length));
				if (output)
					*output = decoded;
				return true;
			}
			if (byte < 0x20)
				return fail(QStringLiteral("Unescaped control character in JSON string"));
			if (byte == '\\')
			{
				if (position >= input.size())
					return fail(QStringLiteral("Unterminated JSON escape"));
				auto const escaped = input.at(position++);
				switch (escaped)
				{
				case '"': decoded.append(QLatin1Char('"')); break;
				case '\\': decoded.append(QLatin1Char('\\')); break;
				case '/': decoded.append(QLatin1Char('/')); break;
				case 'b': decoded.append(QLatin1Char('\b')); break;
				case 'f': decoded.append(QLatin1Char('\f')); break;
				case 'n': decoded.append(QLatin1Char('\n')); break;
				case 'r': decoded.append(QLatin1Char('\r')); break;
				case 't': decoded.append(QLatin1Char('\t')); break;
				case 'u':
				{
					uint codepoint = 0;
					if (!parseHexQuad(&codepoint))
						return false;
					if (codepoint >= 0xd800 && codepoint <= 0xdbff)
					{
						if (position + 2 > input.size() || input.at(position) != '\\' || input.at(position + 1) != 'u')
							return fail(QStringLiteral("Unpaired high surrogate in JSON string"));
						position += 2;
						uint low = 0;
						if (!parseHexQuad(&low))
							return false;
						if (low < 0xdc00 || low > 0xdfff)
							return fail(QStringLiteral("Invalid low surrogate in JSON string"));
						codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
					}
					else if (codepoint >= 0xdc00 && codepoint <= 0xdfff)
					{
						return fail(QStringLiteral("Unpaired low surrogate in JSON string"));
					}
					decoded.append(QString::fromUcs4(&codepoint, 1));
					break;
				}
				default: return fail(QStringLiteral("Invalid JSON escape"));
				}
			}
			else if (byte < 0x80)
			{
				decoded.append(QChar(ushort(byte)));
			}
			else
			{
				auto start = position - 1;
				auto length = (byte < 0xe0) ? 2 : (byte < 0xf0 ? 3 : 4);
				position = start + length;
				decoded.append(QString::fromUtf8(input.constData() + start, length));
			}
		}
		return fail(QStringLiteral("Unterminated JSON string"));
	}

	bool parseHexQuad(uint* value)
	{
		if (position + 4 > input.size())
			return fail(QStringLiteral("Incomplete Unicode escape"));
		uint result = 0;
		for (int i = 0; i < 4; ++i)
		{
			auto const ch = input.at(position++);
			result <<= 4;
			if (ch >= '0' && ch <= '9') result += uint(ch - '0');
			else if (ch >= 'a' && ch <= 'f') result += uint(ch - 'a' + 10);
			else if (ch >= 'A' && ch <= 'F') result += uint(ch - 'A' + 10);
			else return fail(QStringLiteral("Invalid Unicode escape"));
		}
		*value = result;
		return true;
	}

	bool parseNumber()
	{
		auto const start = position;
		if (consume('-') && position >= input.size())
			return fail(QStringLiteral("Incomplete JSON number"));
		if (consume('0'))
		{
			if (position < input.size() && input.at(position) >= '0' && input.at(position) <= '9')
				return fail(QStringLiteral("Leading zero in JSON number"));
		}
		else
		{
			if (position >= input.size() || input.at(position) < '1' || input.at(position) > '9')
				return fail(QStringLiteral("Invalid JSON value"));
			while (position < input.size() && input.at(position) >= '0' && input.at(position) <= '9')
				++position;
		}
		if (consume('.'))
		{
			if (position >= input.size() || input.at(position) < '0' || input.at(position) > '9')
				return fail(QStringLiteral("Missing fraction digits in JSON number"));
			while (position < input.size() && input.at(position) >= '0' && input.at(position) <= '9')
				++position;
		}
		if (position < input.size() && (input.at(position) == 'e' || input.at(position) == 'E'))
		{
			++position;
			if (position < input.size() && (input.at(position) == '+' || input.at(position) == '-'))
				++position;
			if (position >= input.size() || input.at(position) < '0' || input.at(position) > '9')
				return fail(QStringLiteral("Missing exponent digits in JSON number"));
			while (position < input.size() && input.at(position) >= '0' && input.at(position) <= '9')
				++position;
		}

		bool ok = false;
		auto const value = QLocale::c().toDouble(QString::fromLatin1(input.mid(start, position - start)), &ok);
		if (!ok || !std::isfinite(value))
			return fail(QStringLiteral("JSON number is outside the finite IEEE 754 range"));
		return true;
	}

	bool parseLiteral(const char* literal)
	{
		auto const length = int(qstrlen(literal));
		if (input.mid(position, length) != literal)
			return fail(QStringLiteral("Invalid JSON literal"));
		position += length;
		return true;
	}

	void skipSpace()
	{
		while (position < input.size())
		{
			auto const ch = input.at(position);
			if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
				break;
			++position;
		}
	}

	bool consume(char ch)
	{
		if (position < input.size() && input.at(position) == ch)
		{
			++position;
			return true;
		}
		return false;
	}

	bool fail(const QString& message)
	{
		error = QStringLiteral("%1 at byte %2").arg(message).arg(position);
		return false;
	}

	const QByteArray& input;
	int position = 0;
	QString error;
};


class CatalogValidator
{
public:
	explicit CatalogValidator(ImageryCatalogReadResult& result)
	: result(result)
	{}

	void validate(const QJsonObject& root, const QByteArray& bytes)
	{
		static const QSet<QString> fields {
			QStringLiteral("$schema"), QStringLiteral("format"), QStringLiteral("version"),
			QStringLiteral("id"), QStringLiteral("revision"), QStringLiteral("name"),
			QStringLiteral("description"), QStringLiteral("publisher"), QStringLiteral("created"),
			QStringLiteral("updated"), QStringLiteral("catalogLicense"), QStringLiteral("requires"),
			QStringLiteral("resources"), QStringLiteral("extensions"), QStringLiteral("sources")
		};
		checkUnknownFields(root, fields, QStringLiteral("$"), true);

		auto& catalog = result.catalog;
		catalog.original_bytes = bytes;
		catalog.original_object = root;
		catalog.format = requiredString(root, QStringLiteral("format"), QStringLiteral("$.format"), true);
		if (catalog.format != QLatin1String(catalog_format))
			catalogError(QStringLiteral("$.format"), QStringLiteral("Unsupported catalog format"));
		catalog.version = requiredInt(root, QStringLiteral("version"), QStringLiteral("$.version"), true, 1, std::numeric_limits<int>::max());
		if (catalog.version != 1)
			catalogError(QStringLiteral("$.version"), QStringLiteral("Unsupported catalog version"));
		catalog.id = requiredId(root, QStringLiteral("id"), QStringLiteral("$.id"), true);
		catalog.revision = requiredInt(root, QStringLiteral("revision"), QStringLiteral("$.revision"), true, 1, std::numeric_limits<int>::max());
		catalog.name = requiredText(root, QStringLiteral("name"), QStringLiteral("$.name"), true);
		catalog.description = optionalText(root, QStringLiteral("description"), QStringLiteral("$.description"), true);
		catalog.created = optionalDate(root, QStringLiteral("created"), QStringLiteral("$.created"), true);
		catalog.updated = optionalDate(root, QStringLiteral("updated"), QStringLiteral("$.updated"), true);
		optionalText(root, QStringLiteral("catalogLicense"), QStringLiteral("$.catalogLicense"), true);
		optionalUrl(root, QStringLiteral("$schema"), QStringLiteral("$.$schema"), true, false);

		if (root.contains(QStringLiteral("publisher")))
			catalog.publisher = validatePublisher(root.value(QStringLiteral("publisher")), QStringLiteral("$.publisher"));
		if (root.contains(QStringLiteral("extensions")))
		{
			catalog.extensions = objectValue(root.value(QStringLiteral("extensions")), QStringLiteral("$.extensions"), true);
			validateExtensions(catalog.extensions, QStringLiteral("$.extensions"), true);
		}
		if (root.contains(QStringLiteral("requires")))
			catalog.required_capabilities = validateCapabilities(root.value(QStringLiteral("requires")), QStringLiteral("$.requires"), true);
		for (auto const& capability : catalog.required_capabilities)
		{
			if (!supportedCapabilities().contains(capability))
				catalogError(QStringLiteral("$.requires"), QStringLiteral("Required catalog capability is unsupported: %1").arg(capability));
		}

		if (root.contains(QStringLiteral("resources")))
		{
			catalog.resources = objectValue(root.value(QStringLiteral("resources")), QStringLiteral("$.resources"), true);
			validateResources(catalog.resources);
		}

		if (!root.contains(QStringLiteral("sources")) || !root.value(QStringLiteral("sources")).isArray())
		{
			catalogError(QStringLiteral("$.sources"), QStringLiteral("Required member must be an array"));
			return;
		}
		auto const source_array = root.value(QStringLiteral("sources")).toArray();
		if (source_array.isEmpty())
			catalogError(QStringLiteral("$.sources"), QStringLiteral("Catalog must contain at least one source"));
		if (source_array.size() > ImageryCatalogReader::max_sources)
			catalogError(QStringLiteral("$.sources"), QStringLiteral("Catalog exceeds the %1 source limit").arg(ImageryCatalogReader::max_sources));

		QSet<QString> source_ids;
		for (int i = 0; i < source_array.size() && i <= ImageryCatalogReader::max_sources; ++i)
		{
			current_source = i;
			auto const path = QStringLiteral("$.sources[%1]").arg(i);
			if (!source_array.at(i).isObject())
			{
				sourceError(path, QStringLiteral("Source must be an object"));
				continue;
			}
			auto source = validateSource(source_array.at(i).toObject(), path);
			if (source.id.isEmpty())
				continue;
			if (source_ids.contains(source.id))
			{
				catalogError(path + QStringLiteral(".id"), QStringLiteral("Duplicate source ID: %1").arg(source.id));
				continue;
			}
			source_ids.insert(source.id);
			if (source.valid)
				catalog.sources.push_back(source);
		}
		current_source = -1;
	}

private:
	ImagerySourceDefinition validateSource(const QJsonObject& object, const QString& path)
	{
		static const QSet<QString> fields {
			QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("description"),
			QStringLiteral("type"), QStringLiteral("tiles"), QStringLiteral("scheme"),
			QStringLiteral("format"), QStringLiteral("minTileMatrix"), QStringLiteral("maxTileMatrix"),
			QStringLiteral("tileMatrixSetURI"), QStringLiteral("tileMatrixSet"), QStringLiteral("tileMatrixLimits"),
			QStringLiteral("request"), QStringLiteral("requires"), QStringLiteral("category"),
			QStringLiteral("startDate"), QStringLiteral("endDate"), QStringLiteral("coverage"),
			QStringLiteral("notices"), QStringLiteral("registration"), QStringLiteral("extensions")
		};
		auto const issue_start = result.issues.size();
		checkUnknownFields(object, fields, path, false);

		ImagerySourceDefinition source;
		source.original_object = object;
		source.id = requiredId(object, QStringLiteral("id"), path + QStringLiteral(".id"), false);
		source.name = requiredText(object, QStringLiteral("name"), path + QStringLiteral(".name"), false);
		optionalText(object, QStringLiteral("description"), path + QStringLiteral(".description"), false);
		source.type = requiredString(object, QStringLiteral("type"), path + QStringLiteral(".type"), false);
		if (source.type != QLatin1String("raster-tiles"))
			sourceError(path + QStringLiteral(".type"), QStringLiteral("Unsupported source type"));
		source.scheme = requiredString(object, QStringLiteral("scheme"), path + QStringLiteral(".scheme"), false);
		if (source.scheme != QLatin1String("xyz") && source.scheme != QLatin1String("tms"))
			sourceError(path + QStringLiteral(".scheme"), QStringLiteral("Scheme must be xyz or tms"));
		source.format = optionalString(object, QStringLiteral("format"), path + QStringLiteral(".format"), false);
		if (object.contains(QStringLiteral("format")) && source.format.isEmpty())
			sourceError(path + QStringLiteral(".format"), QStringLiteral("Image media type must not be empty"));
		else if (source.format.isEmpty())
			source.format = QStringLiteral("image/png");
		else if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9!#$&^_.+-]+/[A-Za-z0-9!#$&^_.+-]+$")).match(source.format).hasMatch())
			sourceError(path + QStringLiteral(".format"), QStringLiteral("Invalid image media type"));

		validateTiles(object.value(QStringLiteral("tiles")), path + QStringLiteral(".tiles"), source);
		source.min_tile_matrix = optionalString(object, QStringLiteral("minTileMatrix"), path + QStringLiteral(".minTileMatrix"), false);
		source.max_tile_matrix = optionalString(object, QStringLiteral("maxTileMatrix"), path + QStringLiteral(".maxTileMatrix"), false);
		if (object.contains(QStringLiteral("minTileMatrix")) && source.min_tile_matrix.isEmpty())
			sourceError(path + QStringLiteral(".minTileMatrix"), QStringLiteral("Tile matrix identifier must not be empty"));
		if (object.contains(QStringLiteral("maxTileMatrix")) && source.max_tile_matrix.isEmpty())
			sourceError(path + QStringLiteral(".maxTileMatrix"), QStringLiteral("Tile matrix identifier must not be empty"));

		auto const has_uri = object.contains(QStringLiteral("tileMatrixSetURI"));
		auto const has_inline = object.contains(QStringLiteral("tileMatrixSet"));
		if (has_uri == has_inline)
		{
			sourceError(path, QStringLiteral("Source must contain exactly one of tileMatrixSetURI or tileMatrixSet"));
		}
		else if (has_uri)
		{
			source.tile_matrix_set_uri = requiredString(object, QStringLiteral("tileMatrixSetURI"), path + QStringLiteral(".tileMatrixSetURI"), false);
			auto const matrix_set_url = QUrl(source.tile_matrix_set_uri, QUrl::StrictMode);
			if (!matrix_set_url.isValid() || matrix_set_url.isRelative())
				sourceError(path + QStringLiteral(".tileMatrixSetURI"), QStringLiteral("Tile matrix set URI must be an absolute URI"));
			if (source.tile_matrix_set_uri == QLatin1String(web_mercator_quad_uri))
				source.tile_matrix_set = ImageryCatalogReader::webMercatorQuad();
			else
				addUnsupported(source, QStringLiteral("tile-matrix-set.ogc-2.0"), path + QStringLiteral(".tileMatrixSetURI"), QStringLiteral("Unknown tile matrix set URI"));
		}
		else
		{
			source.tile_matrix_set = validateTileMatrixSet(object.value(QStringLiteral("tileMatrixSet")), path + QStringLiteral(".tileMatrixSet"));
		}

		if (object.contains(QStringLiteral("tileMatrixLimits")))
			validateTileMatrixLimits(object.value(QStringLiteral("tileMatrixLimits")), path + QStringLiteral(".tileMatrixLimits"), source);
		validateMatrixRange(source, path);

		if (object.contains(QStringLiteral("request")))
			validateRequest(object.value(QStringLiteral("request")), path + QStringLiteral(".request"), source);
		if (object.contains(QStringLiteral("requires")))
			source.required_capabilities = validateCapabilities(object.value(QStringLiteral("requires")), path + QStringLiteral(".requires"), false);
		for (auto const& capability : source.required_capabilities)
		{
			if (!supportedCapabilities().contains(capability))
				addUnsupported(source, capability, path + QStringLiteral(".requires"), QStringLiteral("Required source capability is unsupported"));
		}

		if (!source.tile_matrix_set.tile_matrices.isEmpty() && !source.tile_matrix_set.is_dyadic)
			addUnsupported(source, QStringLiteral("tile-matrix-set.nondyadic.v1"), path + QStringLiteral(".tileMatrixSet"), QStringLiteral("Tile matrix set is outside the dyadic renderer profile"));

		validatePresentation(object, path);
		if (object.contains(QStringLiteral("registration")))
			validateRegistration(object.value(QStringLiteral("registration")), path + QStringLiteral(".registration"), source);
		if (object.contains(QStringLiteral("extensions")))
			validateExtensions(objectValue(object.value(QStringLiteral("extensions")), path + QStringLiteral(".extensions"), false), path + QStringLiteral(".extensions"), false);

		source.valid = true;
		for (int i = issue_start; i < result.issues.size(); ++i)
		{
			if (result.issues.at(i).type == ImageryCatalogIssue::Type::SourceError)
			{
				source.valid = false;
				break;
			}
		}
		source.supported = source.valid && source.unsupported_capabilities.isEmpty();
		return source;
	}

	void validateTiles(const QJsonValue& value, const QString& path, ImagerySourceDefinition& source)
	{
		if (!value.isArray())
		{
			sourceError(path, QStringLiteral("Tiles must be a nonempty array"));
			return;
		}
		auto const array = value.toArray();
		if (array.isEmpty() || array.size() > ImageryCatalogReader::max_tiles_per_source)
			sourceError(path, QStringLiteral("Tiles must contain between 1 and %1 templates").arg(ImageryCatalogReader::max_tiles_per_source));
		for (int i = 0; i < array.size() && i <= ImageryCatalogReader::max_tiles_per_source; ++i)
		{
			QString const item_path = path + QStringLiteral("[%1]").arg(i);
			if (!array.at(i).isString())
			{
				sourceError(item_path, QStringLiteral("Tile template must be a string"));
				continue;
			}
			auto const text = array.at(i).toString();
			if (text.size() > ImageryCatalogReader::max_url_length)
			{
				sourceError(item_path, QStringLiteral("URL exceeds the %1 character limit").arg(ImageryCatalogReader::max_url_length));
				continue;
			}
			auto normalized_placeholders = text;
			normalized_placeholders.replace(QStringLiteral("${z}"), QStringLiteral("{z}"));
			normalized_placeholders.replace(QStringLiteral("${x}"), QStringLiteral("{x}"));
			normalized_placeholders.replace(QStringLiteral("${y}"), QStringLiteral("{y}"));
			auto url_probe = normalized_placeholders;
			url_probe.replace(QStringLiteral("{z}"), QStringLiteral("0"));
			url_probe.replace(QStringLiteral("{x}"), QStringLiteral("0"));
			url_probe.replace(QStringLiteral("{y}"), QStringLiteral("0"));
			if (!validateHttpUrl(url_probe, item_path, false))
				continue;
			for (auto const& placeholder : { QStringLiteral("{z}"), QStringLiteral("{x}"), QStringLiteral("{y}") })
			{
				if (normalized_placeholders.count(placeholder) != 1)
					sourceError(item_path, QStringLiteral("Tile template must contain exactly one %1 placeholder").arg(placeholder));
			}
			auto without_known = normalized_placeholders;
			without_known.remove(QStringLiteral("{z}"));
			without_known.remove(QStringLiteral("{x}"));
			without_known.remove(QStringLiteral("{y}"));
			if (QRegularExpression(QStringLiteral("\\$?\\{[^}]*\\}")).match(without_known).hasMatch())
				sourceError(item_path, QStringLiteral("Tile template contains an unknown placeholder"));
			source.tiles.push_back(text);
		}
	}

	TileMatrixSetDefinition validateTileMatrixSet(const QJsonValue& value, const QString& path)
	{
		TileMatrixSetDefinition definition;
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Tile matrix set must be an object"));
			return definition;
		}
		auto const object = value.toObject();
		definition.original_object = object;
		static const QSet<QString> fields {
			QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("description"),
			QStringLiteral("keywords"), QStringLiteral("uri"), QStringLiteral("orderedAxes"),
			QStringLiteral("crs"), QStringLiteral("wellKnownScaleSet"), QStringLiteral("boundingBox"),
			QStringLiteral("tileMatrices")
		};
		checkUnknownFields(object, fields, path, false);
		definition.id = requiredId(object, QStringLiteral("id"), path + QStringLiteral(".id"), false);
		optionalText(object, QStringLiteral("title"), path + QStringLiteral(".title"), false);
		optionalText(object, QStringLiteral("description"), path + QStringLiteral(".description"), false);
		optionalUrl(object, QStringLiteral("uri"), path + QStringLiteral(".uri"), false, false);
		optionalUrl(object, QStringLiteral("wellKnownScaleSet"), path + QStringLiteral(".wellKnownScaleSet"), false, false);
		validateStringArray(object.value(QStringLiteral("keywords")), path + QStringLiteral(".keywords"), false, false, 64);
		definition.crs = validateCrs(requiredString(object, QStringLiteral("crs"), path + QStringLiteral(".crs"), false), path + QStringLiteral(".crs"));
		definition.ordered_axes = validateStringArray(object.value(QStringLiteral("orderedAxes")), path + QStringLiteral(".orderedAxes"), false, false, 2);
		if (!definition.ordered_axes.isEmpty() && definition.ordered_axes.size() != 2)
			sourceError(path + QStringLiteral(".orderedAxes"), QStringLiteral("orderedAxes must contain exactly two axes"));
		if (object.contains(QStringLiteral("boundingBox")))
			validateBoundingBox(object.value(QStringLiteral("boundingBox")), path + QStringLiteral(".boundingBox"));
		if (!object.contains(QStringLiteral("tileMatrices")) || !object.value(QStringLiteral("tileMatrices")).isArray())
		{
			sourceError(path + QStringLiteral(".tileMatrices"), QStringLiteral("Required member must be an array"));
			return definition;
		}
		auto const matrices = object.value(QStringLiteral("tileMatrices")).toArray();
		if (matrices.isEmpty() || matrices.size() > ImageryCatalogReader::max_tile_matrices)
			sourceError(path + QStringLiteral(".tileMatrices"), QStringLiteral("Tile matrix count must be between 1 and %1").arg(ImageryCatalogReader::max_tile_matrices));
		QSet<QString> ids;
		for (int i = 0; i < matrices.size() && i <= ImageryCatalogReader::max_tile_matrices; ++i)
		{
			auto matrix = validateTileMatrix(matrices.at(i), path + QStringLiteral(".tileMatrices[%1]").arg(i));
			if (ids.contains(matrix.id))
				sourceError(path + QStringLiteral(".tileMatrices[%1].id").arg(i), QStringLiteral("Duplicate tile matrix ID"));
			ids.insert(matrix.id);
			definition.tile_matrices.push_back(matrix);
		}
		definition.is_dyadic = isDyadic(definition);
		return definition;
	}

	TileMatrixDefinition validateTileMatrix(const QJsonValue& value, const QString& path)
	{
		TileMatrixDefinition matrix;
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Tile matrix must be an object"));
			return matrix;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields {
			QStringLiteral("id"), QStringLiteral("title"), QStringLiteral("description"), QStringLiteral("keywords"),
			QStringLiteral("scaleDenominator"), QStringLiteral("cellSize"), QStringLiteral("pointOfOrigin"),
			QStringLiteral("cornerOfOrigin"), QStringLiteral("tileWidth"), QStringLiteral("tileHeight"),
			QStringLiteral("matrixWidth"), QStringLiteral("matrixHeight"), QStringLiteral("variableMatrixWidths")
		};
		checkUnknownFields(object, fields, path, false);
		matrix.id = requiredString(object, QStringLiteral("id"), path + QStringLiteral(".id"), false);
		optionalText(object, QStringLiteral("title"), path + QStringLiteral(".title"), false);
		optionalText(object, QStringLiteral("description"), path + QStringLiteral(".description"), false);
		validateStringArray(object.value(QStringLiteral("keywords")), path + QStringLiteral(".keywords"), false, false, 64);
		matrix.scale_denominator = requiredPositiveNumber(object, QStringLiteral("scaleDenominator"), path + QStringLiteral(".scaleDenominator"));
		matrix.cell_size = requiredPositiveNumber(object, QStringLiteral("cellSize"), path + QStringLiteral(".cellSize"));
		auto const origin = object.value(QStringLiteral("pointOfOrigin"));
		if (!origin.isArray() || origin.toArray().size() != 2 || !origin.toArray().at(0).isDouble() || !origin.toArray().at(1).isDouble())
			sourceError(path + QStringLiteral(".pointOfOrigin"), QStringLiteral("Point of origin must contain two finite numbers"));
		else
			matrix.point_of_origin = QPointF(origin.toArray().at(0).toDouble(), origin.toArray().at(1).toDouble());
		matrix.corner_of_origin = optionalString(object, QStringLiteral("cornerOfOrigin"), path + QStringLiteral(".cornerOfOrigin"), false);
		if (matrix.corner_of_origin.isEmpty())
			matrix.corner_of_origin = QStringLiteral("topLeft");
		if (matrix.corner_of_origin != QLatin1String("topLeft") && matrix.corner_of_origin != QLatin1String("bottomLeft"))
			sourceError(path + QStringLiteral(".cornerOfOrigin"), QStringLiteral("Unsupported corner of origin"));
		matrix.tile_size.setWidth(requiredInt(object, QStringLiteral("tileWidth"), path + QStringLiteral(".tileWidth"), false, 1, 65536));
		matrix.tile_size.setHeight(requiredInt(object, QStringLiteral("tileHeight"), path + QStringLiteral(".tileHeight"), false, 1, 65536));
		matrix.matrix_width = requiredInteger(object, QStringLiteral("matrixWidth"), path + QStringLiteral(".matrixWidth"), false, 1, max_exact_integer);
		matrix.matrix_height = requiredInteger(object, QStringLiteral("matrixHeight"), path + QStringLiteral(".matrixHeight"), false, 1, max_exact_integer);
		if (object.contains(QStringLiteral("variableMatrixWidths")))
		{
			matrix.has_variable_matrix_widths = true;
			auto const value = object.value(QStringLiteral("variableMatrixWidths"));
			if (!value.isArray() || value.toArray().isEmpty())
				sourceError(path + QStringLiteral(".variableMatrixWidths"), QStringLiteral("Variable matrix widths must be a nonempty array"));
			else
			{
				auto const widths = value.toArray();
				for (int i = 0; i < widths.size(); ++i)
				{
					QString const width_path = path + QStringLiteral(".variableMatrixWidths[%1]").arg(i);
					if (!widths.at(i).isObject())
					{
						sourceError(width_path, QStringLiteral("Variable matrix width entry must be an object"));
						continue;
					}
					auto const width = widths.at(i).toObject();
					static const QSet<QString> width_fields { QStringLiteral("coalesce"), QStringLiteral("minTileRow"), QStringLiteral("maxTileRow") };
					checkUnknownFields(width, width_fields, width_path, false);
					requiredInteger(width, QStringLiteral("coalesce"), width_path + QStringLiteral(".coalesce"), false, 2, max_exact_integer);
					auto const min_row = requiredInteger(width, QStringLiteral("minTileRow"), width_path + QStringLiteral(".minTileRow"), false, 0, max_exact_integer);
					auto const max_row = requiredInteger(width, QStringLiteral("maxTileRow"), width_path + QStringLiteral(".maxTileRow"), false, 0, max_exact_integer);
					if (min_row > max_row || max_row >= matrix.matrix_height)
						sourceError(width_path, QStringLiteral("Variable matrix width rows are outside the matrix"));
				}
			}
		}
		return matrix;
	}

	bool isDyadic(const TileMatrixSetDefinition& definition) const
	{
		if (definition.tile_matrices.isEmpty())
			return false;
		auto const& first = definition.tile_matrices.first();
		for (int i = 0; i < definition.tile_matrices.size(); ++i)
		{
			auto const& matrix = definition.tile_matrices.at(i);
			if (matrix.has_variable_matrix_widths)
				return false;
			bool id_ok = false;
			auto const id = matrix.id.toInt(&id_ok);
			if (!id_ok || QString::number(id) != matrix.id || id != i)
				return false;
			if (matrix.tile_size != first.tile_size
			    || matrix.point_of_origin != first.point_of_origin
			    || matrix.corner_of_origin != first.corner_of_origin)
				return false;
			if (i == 0)
				continue;
			auto const& previous = definition.tile_matrices.at(i - 1);
			auto const tolerance = std::max(std::abs(previous.cell_size) * 1e-9, 1e-12);
			if (std::abs(matrix.cell_size * 2 - previous.cell_size) > tolerance
			    || matrix.matrix_width != previous.matrix_width * 2
			    || matrix.matrix_height != previous.matrix_height * 2)
				return false;
		}
		return true;
	}

	void validateTileMatrixLimits(const QJsonValue& value, const QString& path, ImagerySourceDefinition& source)
	{
		if (!value.isArray())
		{
			sourceError(path, QStringLiteral("Tile matrix limits must be an array"));
			return;
		}
		auto const array = value.toArray();
		if (array.size() > ImageryCatalogReader::max_tile_matrices)
			sourceError(path, QStringLiteral("Too many tile matrix limits"));
		QSet<QString> ids;
		for (int i = 0; i < array.size() && i <= ImageryCatalogReader::max_tile_matrices; ++i)
		{
			QString const item_path = path + QStringLiteral("[%1]").arg(i);
			if (!array.at(i).isObject())
			{
				sourceError(item_path, QStringLiteral("Tile matrix limit must be an object"));
				continue;
			}
			auto const object = array.at(i).toObject();
			static const QSet<QString> fields {
				QStringLiteral("tileMatrix"), QStringLiteral("minTileRow"), QStringLiteral("maxTileRow"),
				QStringLiteral("minTileCol"), QStringLiteral("maxTileCol")
			};
			checkUnknownFields(object, fields, item_path, false);
			TileMatrixLimitsDefinition limit;
			limit.tile_matrix = requiredString(object, QStringLiteral("tileMatrix"), item_path + QStringLiteral(".tileMatrix"), false);
			limit.min_tile_row = requiredInteger(object, QStringLiteral("minTileRow"), item_path + QStringLiteral(".minTileRow"), false, 0, max_exact_integer);
			limit.max_tile_row = requiredInteger(object, QStringLiteral("maxTileRow"), item_path + QStringLiteral(".maxTileRow"), false, 0, max_exact_integer);
			limit.min_tile_col = requiredInteger(object, QStringLiteral("minTileCol"), item_path + QStringLiteral(".minTileCol"), false, 0, max_exact_integer);
			limit.max_tile_col = requiredInteger(object, QStringLiteral("maxTileCol"), item_path + QStringLiteral(".maxTileCol"), false, 0, max_exact_integer);
			if (ids.contains(limit.tile_matrix))
				sourceError(item_path + QStringLiteral(".tileMatrix"), QStringLiteral("Duplicate tile matrix limit"));
			ids.insert(limit.tile_matrix);
			auto const* matrix = findMatrix(source.tile_matrix_set, limit.tile_matrix);
			if (!matrix)
				sourceError(item_path + QStringLiteral(".tileMatrix"), QStringLiteral("Limit refers to an unknown tile matrix"));
			else if (limit.min_tile_row > limit.max_tile_row || limit.min_tile_col > limit.max_tile_col
			         || limit.max_tile_row >= matrix->matrix_height || limit.max_tile_col >= matrix->matrix_width)
				sourceError(item_path, QStringLiteral("Tile matrix limit is outside the matrix dimensions"));
			source.tile_matrix_limits.push_back(limit);
		}
	}

	void validateMatrixRange(ImagerySourceDefinition& source, const QString& path)
	{
		if (source.tile_matrix_set.tile_matrices.isEmpty())
			return;
		if (source.min_tile_matrix.isEmpty())
			source.min_tile_matrix = source.tile_matrix_set.tile_matrices.first().id;
		if (source.max_tile_matrix.isEmpty())
			source.max_tile_matrix = source.tile_matrix_set.tile_matrices.last().id;
		auto const min_index = matrixIndex(source.tile_matrix_set, source.min_tile_matrix);
		auto const max_index = matrixIndex(source.tile_matrix_set, source.max_tile_matrix);
		if (min_index < 0)
			sourceError(path + QStringLiteral(".minTileMatrix"), QStringLiteral("Unknown minimum tile matrix"));
		if (max_index < 0)
			sourceError(path + QStringLiteral(".maxTileMatrix"), QStringLiteral("Unknown maximum tile matrix"));
		if (min_index >= 0 && max_index >= 0 && min_index > max_index)
			sourceError(path, QStringLiteral("Minimum tile matrix follows maximum tile matrix"));
	}

	void validateRequest(const QJsonValue& value, const QString& path, ImagerySourceDefinition& source)
	{
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Request behavior must be an object"));
			return;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields { QStringLiteral("referer"), QStringLiteral("emptyHttpStatusCodes") };
		checkUnknownFields(object, fields, path, false);
		if (object.contains(QStringLiteral("referer")))
		{
			source.request.referer = requiredString(object, QStringLiteral("referer"), path + QStringLiteral(".referer"), false);
			validateHttpUrl(source.request.referer, path + QStringLiteral(".referer"), false);
			for (auto ch : source.request.referer)
			{
				if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
					sourceError(path + QStringLiteral(".referer"), QStringLiteral("Referer contains a control character"));
			}
		}
		if (!object.contains(QStringLiteral("emptyHttpStatusCodes")))
			return;
		auto const codes = object.value(QStringLiteral("emptyHttpStatusCodes"));
		if (!codes.isArray())
		{
			sourceError(path + QStringLiteral(".emptyHttpStatusCodes"), QStringLiteral("HTTP codes must be an array"));
			return;
		}
		if (codes.toArray().size() > ImageryCatalogReader::max_empty_status_codes)
			sourceError(path + QStringLiteral(".emptyHttpStatusCodes"), QStringLiteral("Too many empty-tile HTTP codes"));
		QSet<int> unique;
		for (int i = 0; i < codes.toArray().size() && i <= ImageryCatalogReader::max_empty_status_codes; ++i)
		{
			auto const code = integerValue(codes.toArray().at(i), path + QStringLiteral(".emptyHttpStatusCodes[%1]").arg(i), false, 100, 599);
			if (unique.contains(int(code)))
				sourceError(path + QStringLiteral(".emptyHttpStatusCodes[%1]").arg(i), QStringLiteral("Duplicate HTTP status code"));
			unique.insert(int(code));
			source.request.empty_http_status_codes.push_back(int(code));
		}
	}

	void validatePresentation(const QJsonObject& object, const QString& path)
	{
		auto const category = optionalString(object, QStringLiteral("category"), path + QStringLiteral(".category"), false);
		if (!category.isEmpty() && !QSet<QString>{ QStringLiteral("aerial"), QStringLiteral("satellite"), QStringLiteral("map"), QStringLiteral("elevation"), QStringLiteral("other") }.contains(category))
			sourceError(path + QStringLiteral(".category"), QStringLiteral("Unknown source category"));
		auto const start = optionalDate(object, QStringLiteral("startDate"), path + QStringLiteral(".startDate"), false);
		auto const end = optionalDate(object, QStringLiteral("endDate"), path + QStringLiteral(".endDate"), false);
		if (start.isValid() && end.isValid() && start > end)
			sourceError(path, QStringLiteral("Source startDate follows endDate"));
		if (object.contains(QStringLiteral("coverage")))
		{
			int vertices = 0;
			validateGeometry(object.value(QStringLiteral("coverage")), path + QStringLiteral(".coverage"), vertices);
			if (vertices > ImageryCatalogReader::max_coverage_vertices)
				sourceError(path + QStringLiteral(".coverage"), QStringLiteral("Coverage exceeds the %1 vertex limit").arg(ImageryCatalogReader::max_coverage_vertices));
		}
		if (object.contains(QStringLiteral("notices")))
			validateNotices(object.value(QStringLiteral("notices")), path + QStringLiteral(".notices"));
	}

	void validateRegistration(const QJsonValue& value, const QString& path, ImagerySourceDefinition& source)
	{
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Registration must be an object"));
			return;
		}
		auto const object = value.toObject();
		source.registration.original_object = object;
		static const QSet<QString> fields { QStringLiteral("direction"), QStringLiteral("sourceFrame"), QStringLiteral("targetFrame"), QStringLiteral("operation"), QStringLiteral("provenance") };
		checkUnknownFields(object, fields, path, false);
		source.registration.direction = requiredString(object, QStringLiteral("direction"), path + QStringLiteral(".direction"), false);
		if (source.registration.direction != QLatin1String("source-to-corrected"))
			sourceError(path + QStringLiteral(".direction"), QStringLiteral("Registration direction must be source-to-corrected"));
		source.registration.source_crs = validateFrame(object.value(QStringLiteral("sourceFrame")), path + QStringLiteral(".sourceFrame"), nullptr);
		source.registration.target_crs = validateFrame(object.value(QStringLiteral("targetFrame")), path + QStringLiteral(".targetFrame"), &source.registration.target_frame_id);
		if (!source.tile_matrix_set.crs.isEmpty() && source.registration.source_crs != source.tile_matrix_set.crs)
			sourceError(path + QStringLiteral(".sourceFrame.crs"), QStringLiteral("Registration source frame must match the tile matrix set CRS"));
		if (source.registration.source_crs != source.registration.target_crs)
			sourceError(path + QStringLiteral(".targetFrame.crs"), QStringLiteral("Version 1 registration frames must use the same CRS"));

		if (!object.contains(QStringLiteral("operation")) || !object.value(QStringLiteral("operation")).isObject())
		{
			sourceError(path + QStringLiteral(".operation"), QStringLiteral("Registration operation must be an object"));
			return;
		}
		auto const operation = object.value(QStringLiteral("operation")).toObject();
		auto const type = requiredString(operation, QStringLiteral("type"), path + QStringLiteral(".operation.type"), false);
		if (type == QLatin1String("translation2d"))
		{
			validateTranslation(operation, path + QStringLiteral(".operation"), source);
		}
		else if (type == QLatin1String("affine2d"))
		{
			validateAffine(operation, path + QStringLiteral(".operation"), source);
			addUnsupported(source, QStringLiteral("registration.affine2d.v1"), path + QStringLiteral(".operation"), QStringLiteral("Affine registration rendering is unavailable"));
		}
		else if (type == QLatin1String("gridShift"))
		{
			validateGridShift(operation, path + QStringLiteral(".operation"), source);
			addUnsupported(source, QStringLiteral("registration.grid-shift.v1"), path + QStringLiteral(".operation"), QStringLiteral("Grid-shift registration rendering is unavailable"));
		}
		else
		{
			sourceError(path + QStringLiteral(".operation.type"), QStringLiteral("Unknown registration operation"));
		}
		if (object.contains(QStringLiteral("provenance")))
			validateProvenance(object.value(QStringLiteral("provenance")), path + QStringLiteral(".provenance"));
	}

	void validateTranslation(const QJsonObject& object, const QString& path, ImagerySourceDefinition& source)
	{
		static const QSet<QString> fields { QStringLiteral("type"), QStringLiteral("unit"), QStringLiteral("dx"), QStringLiteral("dy") };
		checkUnknownFields(object, fields, path, false);
		source.registration.operation_type = ImageryRegistration::OperationType::Translation2d;
		source.registration.unit = requiredString(object, QStringLiteral("unit"), path + QStringLiteral(".unit"), false);
		if (source.registration.unit != QLatin1String("crs"))
			sourceError(path + QStringLiteral(".unit"), QStringLiteral("Translation unit must be crs"));
		source.registration.dx = requiredNumber(object, QStringLiteral("dx"), path + QStringLiteral(".dx"));
		source.registration.dy = requiredNumber(object, QStringLiteral("dy"), path + QStringLiteral(".dy"));
	}

	void validateAffine(const QJsonObject& object, const QString& path, ImagerySourceDefinition& source)
	{
		static const QSet<QString> fields {
			QStringLiteral("type"), QStringLiteral("unit"), QStringLiteral("xoff"), QStringLiteral("yoff"),
			QStringLiteral("s11"), QStringLiteral("s12"), QStringLiteral("s21"), QStringLiteral("s22")
		};
		checkUnknownFields(object, fields, path, false);
		source.registration.operation_type = ImageryRegistration::OperationType::Affine2d;
		source.registration.unit = requiredString(object, QStringLiteral("unit"), path + QStringLiteral(".unit"), false);
		if (source.registration.unit != QLatin1String("crs"))
			sourceError(path + QStringLiteral(".unit"), QStringLiteral("Affine unit must be crs"));
		source.registration.xoff = requiredNumber(object, QStringLiteral("xoff"), path + QStringLiteral(".xoff"));
		source.registration.yoff = requiredNumber(object, QStringLiteral("yoff"), path + QStringLiteral(".yoff"));
		source.registration.s11 = requiredNumber(object, QStringLiteral("s11"), path + QStringLiteral(".s11"));
		source.registration.s12 = requiredNumber(object, QStringLiteral("s12"), path + QStringLiteral(".s12"));
		source.registration.s21 = requiredNumber(object, QStringLiteral("s21"), path + QStringLiteral(".s21"));
		source.registration.s22 = requiredNumber(object, QStringLiteral("s22"), path + QStringLiteral(".s22"));
		auto const determinant = source.registration.s11 * source.registration.s22 - source.registration.s12 * source.registration.s21;
		if (!std::isfinite(determinant) || determinant == 0)
			sourceError(path, QStringLiteral("Affine registration must be invertible"));
	}

	void validateGridShift(const QJsonObject& object, const QString& path, ImagerySourceDefinition& source)
	{
		static const QSet<QString> fields { QStringLiteral("type"), QStringLiteral("resource"), QStringLiteral("domain"), QStringLiteral("gridFrame"), QStringLiteral("interpolation") };
		checkUnknownFields(object, fields, path, false);
		source.registration.operation_type = ImageryRegistration::OperationType::GridShift;
		source.registration.resource_id = requiredId(object, QStringLiteral("resource"), path + QStringLiteral(".resource"), false);
		if (!result.catalog.resources.contains(source.registration.resource_id))
			sourceError(path + QStringLiteral(".resource"), QStringLiteral("Grid shift refers to an undeclared resource"));
		auto const domain = requiredString(object, QStringLiteral("domain"), path + QStringLiteral(".domain"), false);
		if (!QSet<QString>{ QStringLiteral("horizontal"), QStringLiteral("vertical"), QStringLiteral("horizontal-and-vertical") }.contains(domain))
			sourceError(path + QStringLiteral(".domain"), QStringLiteral("Unknown grid-shift domain"));
		validateFrame(object.value(QStringLiteral("gridFrame")), path + QStringLiteral(".gridFrame"), nullptr);
		auto const interpolation = requiredString(object, QStringLiteral("interpolation"), path + QStringLiteral(".interpolation"), false);
		if (!QSet<QString>{ QStringLiteral("bilinear"), QStringLiteral("biquadratic"), QStringLiteral("bicubic") }.contains(interpolation))
			sourceError(path + QStringLiteral(".interpolation"), QStringLiteral("Unknown grid-shift interpolation"));
	}

	QString validateFrame(const QJsonValue& value, const QString& path, QString* id)
	{
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Frame must be an object"));
			return {};
		}
		auto const object = value.toObject();
		static const QSet<QString> fields { QStringLiteral("crs"), QStringLiteral("id") };
		checkUnknownFields(object, fields, path, false);
		if (id)
			*id = optionalId(object, QStringLiteral("id"), path + QStringLiteral(".id"), false);
		return validateCrs(requiredString(object, QStringLiteral("crs"), path + QStringLiteral(".crs"), false), path + QStringLiteral(".crs"));
	}

	void validateProvenance(const QJsonValue& value, const QString& path)
	{
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Provenance must be an object"));
			return;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields { QStringLiteral("method"), QStringLiteral("observed"), QStringLiteral("author"), QStringLiteral("rmsError"), QStringLiteral("notes") };
		checkUnknownFields(object, fields, path, false);
		optionalText(object, QStringLiteral("method"), path + QStringLiteral(".method"), false);
		optionalDate(object, QStringLiteral("observed"), path + QStringLiteral(".observed"), false);
		optionalText(object, QStringLiteral("author"), path + QStringLiteral(".author"), false);
		optionalText(object, QStringLiteral("notes"), path + QStringLiteral(".notes"), false);
		if (object.contains(QStringLiteral("rmsError")))
		{
			auto const rms = numberValue(object.value(QStringLiteral("rmsError")), path + QStringLiteral(".rmsError"), false);
			if (rms < 0)
				sourceError(path + QStringLiteral(".rmsError"), QStringLiteral("RMS error must not be negative"));
		}
	}

	void validateResources(const QJsonObject& resources)
	{
		if (resources.size() > ImageryCatalogReader::max_sources)
			catalogError(QStringLiteral("$.resources"), QStringLiteral("Catalog exceeds the resource limit"));
		for (auto it = resources.begin(); it != resources.end(); ++it)
		{
			auto const path = QStringLiteral("$.resources.%1").arg(it.key());
			if (!isId(it.key()))
				catalogError(path, QStringLiteral("Invalid resource ID"));
			if (!it.value().isObject())
			{
				catalogError(path, QStringLiteral("Resource must be an object"));
				continue;
			}
			auto const object = it.value().toObject();
			static const QSet<QString> fields { QStringLiteral("href"), QStringLiteral("mediaType"), QStringLiteral("sha256"), QStringLiteral("size") };
			checkUnknownFields(object, fields, path, true);
			auto const href = requiredString(object, QStringLiteral("href"), path + QStringLiteral(".href"), true);
			if (href.size() > ImageryCatalogReader::max_url_length)
				catalogError(path + QStringLiteral(".href"), QStringLiteral("Resource URL is too long"));
			auto const url = QUrl(href, QUrl::StrictMode);
			if (url.isRelative())
			{
				if (href.startsWith(QLatin1Char('/')) || href.contains(QStringLiteral("..")))
					catalogError(path + QStringLiteral(".href"), QStringLiteral("Unsafe relative resource path"));
			}
			else if (url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty() || url.host().isEmpty())
			{
				catalogError(path + QStringLiteral(".href"), QStringLiteral("Remote resources must use HTTPS without user information"));
			}
			requiredText(object, QStringLiteral("mediaType"), path + QStringLiteral(".mediaType"), true);
			auto const hash = requiredString(object, QStringLiteral("sha256"), path + QStringLiteral(".sha256"), true);
			if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(hash).hasMatch())
				catalogError(path + QStringLiteral(".sha256"), QStringLiteral("Resource SHA-256 must be 64 lowercase hexadecimal characters"));
			requiredInteger(object, QStringLiteral("size"), path + QStringLiteral(".size"), true, 1, 1073741824);
		}
	}

	QJsonObject validatePublisher(const QJsonValue& value, const QString& path)
	{
		auto const object = objectValue(value, path, true);
		static const QSet<QString> fields { QStringLiteral("name"), QStringLiteral("url"), QStringLiteral("contactUrl") };
		checkUnknownFields(object, fields, path, true);
		requiredText(object, QStringLiteral("name"), path + QStringLiteral(".name"), true);
		optionalUrl(object, QStringLiteral("url"), path + QStringLiteral(".url"), true, true);
		optionalUrl(object, QStringLiteral("contactUrl"), path + QStringLiteral(".contactUrl"), true, true);
		return object;
	}

	void validateNotices(const QJsonValue& value, const QString& path)
	{
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Notices must be an object"));
			return;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields {
			QStringLiteral("attributionText"), QStringLiteral("attributionUrl"), QStringLiteral("sourceUrl"),
			QStringLiteral("termsUrl"), QStringLiteral("privacyUrl"), QStringLiteral("notes")
		};
		checkUnknownFields(object, fields, path, false);
		optionalText(object, QStringLiteral("attributionText"), path + QStringLiteral(".attributionText"), false);
		optionalText(object, QStringLiteral("notes"), path + QStringLiteral(".notes"), false);
		for (auto const& name : { QStringLiteral("attributionUrl"), QStringLiteral("sourceUrl"), QStringLiteral("termsUrl"), QStringLiteral("privacyUrl") })
			optionalUrl(object, name, path + QLatin1Char('.') + name, false, true);
	}

	void validateBoundingBox(const QJsonValue& value, const QString& path)
	{
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Bounding box must be an object"));
			return;
		}
		auto const object = value.toObject();
		static const QSet<QString> fields { QStringLiteral("crs"), QStringLiteral("orderedAxes"), QStringLiteral("lowerLeft"), QStringLiteral("upperRight") };
		checkUnknownFields(object, fields, path, false);
		if (object.contains(QStringLiteral("crs")))
			validateCrs(requiredString(object, QStringLiteral("crs"), path + QStringLiteral(".crs"), false), path + QStringLiteral(".crs"));
		validateStringArray(object.value(QStringLiteral("orderedAxes")), path + QStringLiteral(".orderedAxes"), false, false, 2);
		validatePosition(object.value(QStringLiteral("lowerLeft")), path + QStringLiteral(".lowerLeft"), false);
		validatePosition(object.value(QStringLiteral("upperRight")), path + QStringLiteral(".upperRight"), false);
	}

	void validateGeometry(const QJsonValue& value, const QString& path, int& vertices)
	{
		if (!value.isObject())
		{
			sourceError(path, QStringLiteral("Coverage geometry must be an object"));
			return;
		}
		auto const object = value.toObject();
		auto const type = requiredString(object, QStringLiteral("type"), path + QStringLiteral(".type"), false);
		if (type == QLatin1String("GeometryCollection"))
		{
			static const QSet<QString> fields { QStringLiteral("type"), QStringLiteral("geometries") };
			checkUnknownFields(object, fields, path, false);
			if (!object.value(QStringLiteral("geometries")).isArray())
				sourceError(path + QStringLiteral(".geometries"), QStringLiteral("Geometry collection must contain an array"));
			else
			{
				auto const array = object.value(QStringLiteral("geometries")).toArray();
				for (int i = 0; i < array.size() && vertices <= ImageryCatalogReader::max_coverage_vertices; ++i)
					validateGeometry(array.at(i), path + QStringLiteral(".geometries[%1]").arg(i), vertices);
			}
			return;
		}

		static const QSet<QString> fields { QStringLiteral("type"), QStringLiteral("coordinates") };
		checkUnknownFields(object, fields, path, false);
		if (!object.contains(QStringLiteral("coordinates")))
		{
			sourceError(path + QStringLiteral(".coordinates"), QStringLiteral("Geometry coordinates are required"));
			return;
		}
		int coordinate_depth = -1;
		if (type == QLatin1String("Point")) coordinate_depth = 0;
		else if (type == QLatin1String("MultiPoint") || type == QLatin1String("LineString")) coordinate_depth = 1;
		else if (type == QLatin1String("MultiLineString") || type == QLatin1String("Polygon")) coordinate_depth = 2;
		else if (type == QLatin1String("MultiPolygon")) coordinate_depth = 3;
		else sourceError(path + QStringLiteral(".type"), QStringLiteral("Unknown GeoJSON geometry type"));
		if (coordinate_depth >= 0)
		{
			QString const coordinates_path = path + QStringLiteral(".coordinates");
			validateCoordinates(object.value(QStringLiteral("coordinates")), coordinates_path, coordinate_depth, vertices);
			if (object.value(QStringLiteral("coordinates")).isArray())
				validateGeometryShape(type, object.value(QStringLiteral("coordinates")).toArray(), coordinates_path);
		}
	}

	void validateGeometryShape(const QString& type, const QJsonArray& coordinates, const QString& path)
	{
		if (type == QLatin1String("LineString") && coordinates.size() < 2)
			sourceError(path, QStringLiteral("LineString must contain at least two positions"));
		else if (type == QLatin1String("MultiLineString"))
		{
			for (int i = 0; i < coordinates.size(); ++i)
				if (coordinates.at(i).isArray() && coordinates.at(i).toArray().size() < 2)
					sourceError(path + QStringLiteral("[%1]").arg(i), QStringLiteral("LineString must contain at least two positions"));
		}
		else if (type == QLatin1String("Polygon"))
		{
			validatePolygonRings(coordinates, path);
		}
		else if (type == QLatin1String("MultiPolygon"))
		{
			for (int i = 0; i < coordinates.size(); ++i)
				if (coordinates.at(i).isArray())
					validatePolygonRings(coordinates.at(i).toArray(), path + QStringLiteral("[%1]").arg(i));
		}
	}

	void validatePolygonRings(const QJsonArray& rings, const QString& path)
	{
		for (int i = 0; i < rings.size(); ++i)
		{
			if (!rings.at(i).isArray())
				continue;
			auto const ring = rings.at(i).toArray();
			QString const ring_path = path + QStringLiteral("[%1]").arg(i);
			if (ring.size() < 4)
				sourceError(ring_path, QStringLiteral("Polygon ring must contain at least four positions"));
			else if (ring.first() != ring.last())
				sourceError(ring_path, QStringLiteral("Polygon ring must be closed"));
		}
	}

	void validateCoordinates(const QJsonValue& value, const QString& path, int depth, int& vertices)
	{
		if (!value.isArray())
		{
			sourceError(path, QStringLiteral("GeoJSON coordinates must be arrays"));
			return;
		}
		auto const array = value.toArray();
		if (array.isEmpty())
			sourceError(path, QStringLiteral("GeoJSON coordinate array must not be empty"));
		if (depth == 0)
		{
			if (array.size() < 2 || array.size() > 3 || !array.at(0).isDouble() || !array.at(1).isDouble()
			    || (array.size() == 3 && !array.at(2).isDouble()))
			{
				sourceError(path, QStringLiteral("GeoJSON position must contain two or three finite numbers"));
				return;
			}
			auto const longitude = array.at(0).toDouble();
			auto const latitude = array.at(1).toDouble();
			if (longitude < -180 || longitude > 180 || latitude < -90 || latitude > 90)
				sourceError(path, QStringLiteral("Coverage position is outside WGS84 longitude/latitude bounds"));
			++vertices;
			return;
		}
		for (int i = 0; i < array.size() && vertices <= ImageryCatalogReader::max_coverage_vertices; ++i)
			validateCoordinates(array.at(i), path + QStringLiteral("[%1]").arg(i), depth - 1, vertices);
	}

	QString validateCrs(const QString& crs, const QString& path)
	{
		static const QRegularExpression short_form(QStringLiteral("^EPSG:([1-9][0-9]*)$"));
		static const QRegularExpression uri_form(QStringLiteral("^https?://www\\.opengis\\.net/def/crs/EPSG/(?:0|[0-9.]+)/([1-9][0-9]*)$"));
		auto match = short_form.match(crs);
		if (!match.hasMatch())
			match = uri_form.match(crs);
		if (!match.hasMatch())
		{
			sourceError(path, QStringLiteral("CRS must be an EPSG code or equivalent OGC URI"));
			return {};
		}
		auto const normalized = QStringLiteral("EPSG:%1").arg(match.captured(1));
		auto* context = proj_context_create();
		auto* object = proj_create(context, normalized.toLatin1().constData());
		if (!object)
			sourceError(path, QStringLiteral("CRS is not recognized by PROJ"));
		proj_destroy(object);
		proj_context_destroy(context);
		return normalized;
	}

	QStringList validateCapabilities(const QJsonValue& value, const QString& path, bool catalog_level)
	{
		auto values = validateStringArray(value, path, catalog_level, true, 64);
		for (auto const& capability : values)
		{
			if (!isId(capability))
				addError(catalog_level, path, QStringLiteral("Invalid capability identifier"));
		}
		return values;
	}

	QStringList validateStringArray(const QJsonValue& value, const QString& path, bool catalog_level, bool unique, int maximum)
	{
		QStringList strings;
		if (value.isUndefined())
			return strings;
		if (!value.isArray())
		{
			addError(catalog_level, path, QStringLiteral("Expected an array of strings"));
			return strings;
		}
		auto const array = value.toArray();
		if (array.size() > maximum)
			addError(catalog_level, path, QStringLiteral("Array exceeds the %1 item limit").arg(maximum));
		QSet<QString> seen;
		for (int i = 0; i < array.size() && i <= maximum; ++i)
		{
			if (!array.at(i).isString() || array.at(i).toString().isEmpty())
			{
				addError(catalog_level, path + QStringLiteral("[%1]").arg(i), QStringLiteral("Expected a nonempty string"));
				continue;
			}
			auto const string = array.at(i).toString();
			if (unique && seen.contains(string))
				addError(catalog_level, path + QStringLiteral("[%1]").arg(i), QStringLiteral("Duplicate array value"));
			seen.insert(string);
			strings.push_back(string);
		}
		return strings;
	}

	void validateExtensions(const QJsonObject& object, const QString& path, bool catalog_level)
	{
		static const QRegularExpression namespaced(QStringLiteral("^[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)+$"));
		for (auto it = object.begin(); it != object.end(); ++it)
		{
			if (!namespaced.match(it.key()).hasMatch())
				addError(catalog_level, path + QLatin1Char('.') + it.key(), QStringLiteral("Extension key must be reverse-DNS namespaced"));
		}
	}

	void checkUnknownFields(const QJsonObject& object, const QSet<QString>& allowed, const QString& path, bool catalog_level)
	{
		for (auto it = object.begin(); it != object.end(); ++it)
		{
			if (!allowed.contains(it.key()))
				addError(catalog_level, path + QLatin1Char('.') + it.key(), QStringLiteral("Unknown member"));
		}
	}

	QString requiredText(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level)
	{
		auto const text = requiredString(object, name, path, catalog_level);
		if (text.isEmpty())
			addError(catalog_level, path, QStringLiteral("Text must not be empty"));
		return text;
	}

	QString optionalText(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level)
	{
		if (!object.contains(name))
			return {};
		auto const text = requiredString(object, name, path, catalog_level);
		if (text.isEmpty())
			addError(catalog_level, path, QStringLiteral("Text must not be empty"));
		return text;
	}

	QString requiredId(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level)
	{
		auto const id = requiredString(object, name, path, catalog_level);
		if (!id.isEmpty() && !isId(id))
			addError(catalog_level, path, QStringLiteral("Invalid identifier"));
		return id;
	}

	QString optionalId(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level)
	{
		if (!object.contains(name))
			return {};
		return requiredId(object, name, path, catalog_level);
	}

	bool isId(const QString& id) const
	{
		return id.size() <= 255 && QRegularExpression(QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9])?$")).match(id).hasMatch();
	}

	QString requiredString(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level)
	{
		if (!object.contains(name) || !object.value(name).isString())
		{
			addError(catalog_level, path, QStringLiteral("Required member must be a string"));
			return {};
		}
		return object.value(name).toString();
	}

	QString optionalString(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level)
	{
		if (!object.contains(name))
			return {};
		return requiredString(object, name, path, catalog_level);
	}

	QDate optionalDate(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level)
	{
		if (!object.contains(name))
			return {};
		auto const text = requiredString(object, name, path, catalog_level);
		auto const date = QDate::fromString(text, Qt::ISODate);
		if (!date.isValid() || date.toString(Qt::ISODate) != text)
			addError(catalog_level, path, QStringLiteral("Date must use YYYY-MM-DD ISO 8601 form"));
		return date;
	}

	QString optionalUrl(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level, bool http_only)
	{
		if (!object.contains(name))
			return {};
		auto const text = requiredString(object, name, path, catalog_level);
		if (http_only)
			validateHttpUrl(text, path, catalog_level);
		else
		{
			auto const url = QUrl(text, QUrl::StrictMode);
			if (!url.isValid() || url.isRelative() || text.size() > ImageryCatalogReader::max_url_length)
				addError(catalog_level, path, QStringLiteral("Invalid absolute URL"));
		}
		return text;
	}

	bool validateHttpUrl(const QString& text, const QString& path, bool catalog_level)
	{
		if (text.size() > ImageryCatalogReader::max_url_length)
		{
			addError(catalog_level, path, QStringLiteral("URL exceeds the %1 character limit").arg(ImageryCatalogReader::max_url_length));
			return false;
		}
		auto const url = QUrl(text, QUrl::StrictMode);
		if (!url.isValid() || url.isRelative() || url.host().isEmpty()
		    || (url.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) != 0
		        && url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0)
		    || !url.userInfo().isEmpty())
		{
			addError(catalog_level, path, QStringLiteral("URL must be absolute HTTP(S) without user information"));
			return false;
		}
		for (auto ch : text)
		{
			if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
			{
				addError(catalog_level, path, QStringLiteral("URL contains a control character"));
				return false;
			}
		}
		return true;
	}

	double requiredPositiveNumber(const QJsonObject& object, const QString& name, const QString& path)
	{
		auto const value = requiredNumber(object, name, path);
		if (!(value > 0))
			sourceError(path, QStringLiteral("Number must be positive"));
		return value;
	}

	double requiredNumber(const QJsonObject& object, const QString& name, const QString& path)
	{
		if (!object.contains(name))
		{
			sourceError(path, QStringLiteral("Required numeric member is missing"));
			return 0;
		}
		return numberValue(object.value(name), path, false);
	}

	double numberValue(const QJsonValue& value, const QString& path, bool catalog_level)
	{
		if (!value.isDouble() || !std::isfinite(value.toDouble()))
		{
			addError(catalog_level, path, QStringLiteral("Expected a finite number"));
			return 0;
		}
		return value.toDouble();
	}

	int requiredInt(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level, int minimum, int maximum)
	{
		return int(requiredInteger(object, name, path, catalog_level, minimum, maximum));
	}

	qint64 requiredInteger(const QJsonObject& object, const QString& name, const QString& path, bool catalog_level, double minimum, double maximum)
	{
		if (!object.contains(name))
		{
			addError(catalog_level, path, QStringLiteral("Required integer member is missing"));
			return qint64(minimum);
		}
		return integerValue(object.value(name), path, catalog_level, minimum, maximum);
	}

	qint64 integerValue(const QJsonValue& value, const QString& path, bool catalog_level, double minimum, double maximum)
	{
		auto const number = numberValue(value, path, catalog_level);
		if (std::floor(number) != number || number < minimum || number > maximum || std::abs(number) > max_exact_integer)
		{
			addError(catalog_level, path, QStringLiteral("Integer is outside its permitted exact IEEE 754 range"));
			return qint64(minimum);
		}
		return qint64(number);
	}

	QJsonObject objectValue(const QJsonValue& value, const QString& path, bool catalog_level)
	{
		if (!value.isObject())
		{
			addError(catalog_level, path, QStringLiteral("Expected an object"));
			return {};
		}
		return value.toObject();
	}

	void validatePosition(const QJsonValue& value, const QString& path, bool catalog_level)
	{
		if (!value.isArray() || value.toArray().size() != 2
		    || !value.toArray().at(0).isDouble() || !value.toArray().at(1).isDouble())
			addError(catalog_level, path, QStringLiteral("Expected a two-number position"));
	}

	const TileMatrixDefinition* findMatrix(const TileMatrixSetDefinition& definition, const QString& id) const
	{
		for (auto const& matrix : definition.tile_matrices)
			if (matrix.id == id)
				return &matrix;
		return nullptr;
	}

	int matrixIndex(const TileMatrixSetDefinition& definition, const QString& id) const
	{
		for (int i = 0; i < definition.tile_matrices.size(); ++i)
			if (definition.tile_matrices.at(i).id == id)
				return i;
		return -1;
	}

	void addUnsupported(ImagerySourceDefinition& source, const QString& capability, const QString& path, const QString& message)
	{
		if (!source.unsupported_capabilities.contains(capability))
			source.unsupported_capabilities.push_back(capability);
		result.issues.push_back({ ImageryCatalogIssue::Type::UnsupportedSource, path, message + QStringLiteral(": ") + capability, current_source });
	}

	void addError(bool catalog_level, const QString& path, const QString& message)
	{
		if (catalog_level) catalogError(path, message);
		else sourceError(path, message);
	}

	void catalogError(const QString& path, const QString& message)
	{
		result.issues.push_back({ ImageryCatalogIssue::Type::CatalogError, path, message, current_source });
	}

	void sourceError(const QString& path, const QString& message)
	{
		result.issues.push_back({ ImageryCatalogIssue::Type::SourceError, path, message, current_source });
	}

	static const QSet<QString>& supportedCapabilities()
	{
		static const QSet<QString> capabilities {
			QStringLiteral("tile-matrix-set.ogc-2.0"),
			QStringLiteral("tile-matrix-set.dyadic.v1"),
			QStringLiteral("registration.translation2d.v1")
		};
		return capabilities;
	}

	ImageryCatalogReadResult& result;
	int current_source = -1;
};

}  // namespace


bool ImageryCatalogReadResult::accepted() const
{
	return !hasCatalogErrors() && !catalog.sources.isEmpty();
}


bool ImageryCatalogReadResult::hasCatalogErrors() const
{
	for (auto const& issue : issues)
		if (issue.type == ImageryCatalogIssue::Type::CatalogError)
			return true;
	return false;
}


QString ImageryCatalogReader::fileExtension()
{
	return QStringLiteral("oic");
}


ImageryCatalogReadResult ImageryCatalogReader::read(const QByteArray& bytes)
{
	ImageryCatalogReadResult result;
	result.catalog.document_sha256 = ImageryJsonCanonicalizer::sha256(bytes);
	if (bytes.size() > max_document_size)
	{
		result.issues.push_back({ ImageryCatalogIssue::Type::CatalogError,
		                          QStringLiteral("$"),
		                          QStringLiteral("Catalog exceeds the %1-byte limit").arg(max_document_size),
		                          -1 });
		return result;
	}

	JsonPreflight preflight(bytes);
	if (!preflight.validate())
	{
		result.issues.push_back({ ImageryCatalogIssue::Type::CatalogError,
		                          QStringLiteral("$"),
		                          preflight.errorString(),
		                          -1 });
		return result;
	}

	QJsonParseError parse_error;
	auto const document = QJsonDocument::fromJson(bytes, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !document.isObject())
	{
		result.issues.push_back({ ImageryCatalogIssue::Type::CatalogError,
		                          QStringLiteral("$"),
		                          parse_error.error == QJsonParseError::NoError
		                            ? QStringLiteral("Catalog root must be an object")
		                            : parse_error.errorString(),
		                          -1 });
		return result;
	}

	CatalogValidator validator(result);
	validator.validate(document.object(), bytes);
	for (int i = result.catalog.sources.size() - 1; i >= 0; --i)
	{
		ImagerySourceFingerprints fingerprints;
		QString error;
		if (!ImagerySourceFingerprint::calculate(result.catalog.sources.at(i), &fingerprints, &error))
		{
			result.issues.push_back({ ImageryCatalogIssue::Type::SourceError,
			                          QStringLiteral("$.sources"),
			                          QStringLiteral("Unable to fingerprint source: %1").arg(error),
			                          -1 });
			result.catalog.sources.removeAt(i);
			continue;
		}
		result.catalog.sources[i].full_fingerprint = fingerprints.full;
		result.catalog.sources[i].operational_fingerprint = fingerprints.operational;
	}
	return result;
}


TileMatrixSetDefinition ImageryCatalogReader::webMercatorQuad()
{
	TileMatrixSetDefinition definition;
	definition.id = QStringLiteral("WebMercatorQuad");
	definition.crs = QStringLiteral("EPSG:3857");
	definition.ordered_axes = QStringList { QStringLiteral("X"), QStringLiteral("Y") };
	definition.is_dyadic = true;

	QJsonArray matrices;
	constexpr double origin = 20037508.342789244;
	constexpr double base_cell_size = 156543.03392804097;
	constexpr double base_scale_denominator = 559082264.0287178;
	for (int zoom = 0; zoom <= 24; ++zoom)
	{
		auto const divisor = double(qint64(1) << zoom);
		TileMatrixDefinition matrix;
		matrix.id = QString::number(zoom);
		matrix.scale_denominator = base_scale_denominator / divisor;
		matrix.cell_size = base_cell_size / divisor;
		matrix.point_of_origin = QPointF(-origin, origin);
		matrix.corner_of_origin = QStringLiteral("topLeft");
		matrix.tile_size = QSize(256, 256);
		matrix.matrix_width = qint64(1) << zoom;
		matrix.matrix_height = qint64(1) << zoom;
		definition.tile_matrices.push_back(matrix);

		matrices.push_back(QJsonObject {
			{ QStringLiteral("id"), matrix.id },
			{ QStringLiteral("scaleDenominator"), matrix.scale_denominator },
			{ QStringLiteral("cellSize"), matrix.cell_size },
			{ QStringLiteral("pointOfOrigin"), QJsonArray { -origin, origin } },
			{ QStringLiteral("cornerOfOrigin"), matrix.corner_of_origin },
			{ QStringLiteral("tileWidth"), matrix.tile_size.width() },
			{ QStringLiteral("tileHeight"), matrix.tile_size.height() },
			{ QStringLiteral("matrixWidth"), double(matrix.matrix_width) },
			{ QStringLiteral("matrixHeight"), double(matrix.matrix_height) }
		});
	}
	definition.original_object = QJsonObject {
		{ QStringLiteral("id"), definition.id },
		{ QStringLiteral("crs"), definition.crs },
		{ QStringLiteral("orderedAxes"), QJsonArray { QStringLiteral("X"), QStringLiteral("Y") } },
		{ QStringLiteral("tileMatrices"), matrices }
	};
	return definition;
}


}  // namespace OpenOrienteering
