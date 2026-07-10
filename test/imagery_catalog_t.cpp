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

#include <functional>

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "test_config.h"

#include "gdal/imagery_catalog_reader.h"
#include "gdal/imagery_catalog_store.h"
#include "gdal/imagery_json_canonicalizer.h"
#include "gdal/imagery_source_fingerprint.h"
#include "gdal/imagery_source_resolver.h"

namespace OpenOrienteering {

namespace {

QByteArray fixture(const QString& relative_path)
{
	QFile file(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR) + QStringLiteral("/data/imagery-catalogs/") + relative_path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	return file.readAll();
}


QJsonObject minimalCatalogObject()
{
	return QJsonDocument::fromJson(fixture(QStringLiteral("valid/minimal.oic"))).object();
}


QByteArray json(const QJsonObject& object)
{
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}


QByteArray changedCatalog(const std::function<void(QJsonObject&)>& change)
{
	auto object = minimalCatalogObject();
	change(object);
	return json(object);
}


QByteArray changedSource(const std::function<void(QJsonObject&)>& change)
{
	return changedCatalog([&change](QJsonObject& catalog) {
		auto sources = catalog.value(QStringLiteral("sources")).toArray();
		auto source = sources.at(0).toObject();
		change(source);
		sources.replace(0, source);
		catalog.insert(QStringLiteral("sources"), sources);
	});
}


bool hasIssue(const ImageryCatalogReadResult& result, ImageryCatalogIssue::Type type, const QString& fragment = {})
{
	for (auto const& issue : result.issues)
	{
		if (issue.type == type && (fragment.isEmpty() || issue.message.contains(fragment) || issue.path.contains(fragment)))
			return true;
	}
	return false;
}


QJsonObject translationRegistration()
{
	return QJsonObject {
		{ QStringLiteral("direction"), QStringLiteral("source-to-corrected") },
		{ QStringLiteral("sourceFrame"), QJsonObject { { QStringLiteral("crs"), QStringLiteral("EPSG:3857") } } },
		{ QStringLiteral("targetFrame"), QJsonObject { { QStringLiteral("crs"), QStringLiteral("EPSG:3857") } } },
		{ QStringLiteral("operation"), QJsonObject {
			{ QStringLiteral("type"), QStringLiteral("translation2d") },
			{ QStringLiteral("unit"), QStringLiteral("crs") },
			{ QStringLiteral("dx"), -0.42 },
			{ QStringLiteral("dy"), 0.17 }
		} }
	};
}

}  // namespace


class ImageryCatalogTest : public QObject
{
	Q_OBJECT

private slots:
	void validFixtures()
	{
		auto const minimal_bytes = fixture(QStringLiteral("valid/minimal.oic"));
		QVERIFY(!minimal_bytes.isEmpty());
		auto const minimal = ImageryCatalogReader::read(minimal_bytes);
		QVERIFY2(minimal.accepted(), qPrintable(minimal.issues.isEmpty() ? QString{} : minimal.issues.first().message));
		QCOMPARE(minimal.catalog.original_bytes, minimal_bytes);
		QCOMPARE(minimal.catalog.id, QStringLiteral("org.example.imagery.minimal"));
		QCOMPARE(minimal.catalog.sources.size(), 1);
		QVERIFY(minimal.catalog.sources.first().supported);
		QCOMPARE(minimal.catalog.sources.first().format, QStringLiteral("image/png"));
		QCOMPARE(minimal.catalog.sources.first().tile_matrix_set.crs, QStringLiteral("EPSG:3857"));
		QCOMPARE(minimal.catalog.sources.first().tile_matrix_set.tile_matrices.size(), 25);
		QCOMPARE(minimal.catalog.sources.first().full_fingerprint.size(), 64);
		QCOMPARE(minimal.catalog.sources.first().operational_fingerprint.size(), 64);
		QCOMPARE(minimal.catalog.document_sha256, ImageryJsonCanonicalizer::sha256(minimal_bytes));

		auto const custom_bytes = fixture(QStringLiteral("valid/custom-dyadic-epsg2927.oic"));
		auto const custom = ImageryCatalogReader::read(custom_bytes);
		QVERIFY2(custom.accepted(), qPrintable(custom.issues.isEmpty() ? QString{} : custom.issues.first().message));
		QCOMPARE(custom.catalog.original_bytes, custom_bytes);
		QCOMPARE(custom.catalog.sources.first().tile_matrix_set.crs, QStringLiteral("EPSG:2927"));
		QVERIFY(custom.catalog.sources.first().tile_matrix_set.is_dyadic);
		QCOMPARE(custom.catalog.sources.first().request.empty_http_status_codes, QVector<int>({ 204, 404 }));
		QCOMPARE(custom.catalog.sources.first().registration.operation_type, ImageryRegistration::OperationType::Translation2d);

		auto const unsupported = ImageryCatalogReader::read(fixture(QStringLiteral("unsupported/non-dyadic-matrix-set.oic")));
		QVERIFY(unsupported.accepted());
		QCOMPARE(unsupported.catalog.sources.size(), 1);
		QVERIFY(!unsupported.catalog.sources.first().supported);
		QVERIFY(hasIssue(unsupported, ImageryCatalogIssue::Type::UnsupportedSource, QStringLiteral("nondyadic")));
	}

	void pugetSoundExampleValidates()
	{
		QFile file(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR) + QStringLiteral("/../examples/puget-sound.oic"));
		QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
		auto const example = ImageryCatalogReader::read(file.readAll());
		QVERIFY2(example.issues.isEmpty(), qPrintable(example.issues.isEmpty() ? QString{} : example.issues.first().message));
		QVERIFY(example.accepted());
		QCOMPARE(example.catalog.sources.size(), 5);
		for (auto const& source : example.catalog.sources)
		{
			QVERIFY(source.supported);
			QCOMPARE(source.min_tile_matrix, QStringLiteral("10"));
			if (source.id.startsWith(QStringLiteral("king-county-")))
				QCOMPARE(source.max_tile_matrix, QStringLiteral("20"));
			else if (source.id == QLatin1String("pierce-county-ortho"))
				QCOMPARE(source.max_tile_matrix, QStringLiteral("10"));
		}
		for (auto const& source : example.catalog.sources.mid(0, 4))
			QCOMPARE(source.request.empty_http_status_codes, QVector<int>({ 404 }));
	}

	void fingerprints()
	{
		auto const base_bytes = fixture(QStringLiteral("valid/minimal.oic"));
		auto const base = ImageryCatalogReader::read(base_bytes);
		QVERIFY(base.accepted());
		auto const base_source = base.catalog.sources.first();
		QCOMPARE(base_source.full_fingerprint,
		         QByteArray("231ea73d1cb066e5d67a56981b25ed649279ad4aec79d1569bf2f591d6dedbf4"));
		QCOMPARE(base_source.operational_fingerprint,
		         QByteArray("f76f7b6b37eb68278ddc458cd72d1ebd40d23b7b5e8e1e4fa3edf3d5dfe46ed5"));

		auto aliases = ImageryCatalogReader::read(changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral("HTTPS://TILES.EXAMPLE.TEST:443/aerial/${z}/${x}/${y}.png")
			});
		}));
		QVERIFY(aliases.accepted());
		QCOMPARE(aliases.catalog.sources.first().full_fingerprint, base_source.full_fingerprint);
		QCOMPARE(aliases.catalog.sources.first().operational_fingerprint, base_source.operational_fingerprint);

		auto inline_matrix = ImageryCatalogReader::read(changedSource([](QJsonObject& source) {
			source.remove(QStringLiteral("tileMatrixSetURI"));
			source.insert(QStringLiteral("tileMatrixSet"), ImageryCatalogReader::webMercatorQuad().original_object);
		}));
		QVERIFY(inline_matrix.accepted());
		QCOMPARE(inline_matrix.catalog.sources.first().full_fingerprint, base_source.full_fingerprint);
		QCOMPARE(inline_matrix.catalog.sources.first().operational_fingerprint, base_source.operational_fingerprint);

		auto descriptive = ImageryCatalogReader::read(changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("name"), QStringLiteral("Renamed source"));
			source.insert(QStringLiteral("notices"), QJsonObject {
				{ QStringLiteral("attributionText"), QStringLiteral("New attribution") }
			});
		}));
		QVERIFY(descriptive.accepted());
		QVERIFY(descriptive.catalog.sources.first().full_fingerprint != base_source.full_fingerprint);
		QCOMPARE(descriptive.catalog.sources.first().operational_fingerprint, base_source.operational_fingerprint);

		auto operational = ImageryCatalogReader::read(changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral("https://other.example.test/aerial/{z}/{x}/{y}.png")
			});
		}));
		QVERIFY(operational.accepted());
		QVERIFY(operational.catalog.sources.first().full_fingerprint != base_source.full_fingerprint);
		QVERIFY(operational.catalog.sources.first().operational_fingerprint != base_source.operational_fingerprint);

		auto registered = ImageryCatalogReader::read(changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("registration"), translationRegistration());
		}));
		QVERIFY(registered.accepted());
		QVERIFY(registered.catalog.sources.first().full_fingerprint != base_source.full_fingerprint);
		QVERIFY(registered.catalog.sources.first().operational_fingerprint != base_source.operational_fingerprint);

		auto reordered_document = QJsonDocument(minimalCatalogObject()).toJson(QJsonDocument::Compact);
		auto reordered = ImageryCatalogReader::read(reordered_document);
		QVERIFY(reordered.accepted());
		QCOMPARE(reordered.catalog.sources.first().full_fingerprint, base_source.full_fingerprint);
		QCOMPARE(reordered.catalog.sources.first().operational_fingerprint, base_source.operational_fingerprint);
		QVERIFY(reordered.catalog.document_sha256 != base.catalog.document_sha256);
	}

	void numericFingerprintEquivalence()
	{
		auto first_bytes = fixture(QStringLiteral("valid/custom-dyadic-epsg2927.oic"));
		auto second_bytes = first_bytes;
		QVERIFY(second_bytes.contains("-0.42"));
		second_bytes.replace("-0.42", "-4.2e-1");
		auto const first = ImageryCatalogReader::read(first_bytes);
		auto const second = ImageryCatalogReader::read(second_bytes);
		QVERIFY(first.accepted());
		QVERIFY(second.accepted());
		QCOMPARE(first.catalog.sources.first().full_fingerprint, second.catalog.sources.first().full_fingerprint);
		QCOMPARE(first.catalog.sources.first().operational_fingerprint, second.catalog.sources.first().operational_fingerprint);
		QVERIFY(first.catalog.document_sha256 != second.catalog.document_sha256);
	}

	void sourceResolution()
	{
		auto const valid = ImageryCatalogReader::read(fixture(QStringLiteral("valid/custom-dyadic-epsg2927.oic")));
		QVERIFY(valid.accepted());
		auto const resolved = ImagerySourceResolver::resolve(valid.catalog.sources.first());
		QVERIFY2(resolved.error.isEmpty(), qPrintable(resolved.error));
		QCOMPARE(resolved.source.tile_matrix_set.crs, QStringLiteral("EPSG:2927"));
		QCOMPARE(resolved.source.request.referer, QStringLiteral("https://www.example.org/maps/"));
		QCOMPARE(resolved.source.registration.operation_type, ImageryRegistration::OperationType::Translation2d);
		QCOMPARE(resolved.source.operational_fingerprint, valid.catalog.sources.first().operational_fingerprint);

		auto const unsupported = ImageryCatalogReader::read(fixture(QStringLiteral("unsupported/non-dyadic-matrix-set.oic")));
		QVERIFY(unsupported.accepted());
		QVERIFY(!ImagerySourceResolver::resolve(unsupported.catalog.sources.first()).error.isEmpty());
	}

	void catalogStore()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		ImageryCatalogStore store(directory.filePath(QStringLiteral("catalog-store")));
		auto const bytes = fixture(QStringLiteral("valid/minimal.oic"));
		auto const catalog = ImageryCatalogReader::read(bytes);
		QVERIFY(catalog.accepted());
		QString error;
		QVERIFY2(store.install(catalog, QStringLiteral("https://example.test/catalog"), QByteArray("etag-1"), QByteArray("yesterday"), &error), qPrintable(error));
		auto installed = store.catalogs(&error);
		QCOMPARE(installed.size(), 1);
		QCOMPARE(installed.first().read_result.catalog.original_bytes, bytes);
		QCOMPARE(installed.first().state.origin, QStringLiteral("https://example.test/catalog"));
		QCOMPARE(installed.first().state.etag, QByteArray("etag-1"));
		QCOMPARE(installed.first().state.last_modified, QByteArray("yesterday"));
		QVERIFY(installed.first().state.installed_at.isValid());
		QVERIFY(QFileInfo(QDir(installed.first().directory).filePath(QStringLiteral("catalog.oic"))).isFile());
		QVERIFY(!store.directoryKey(QStringLiteral("../../unsafe catalog")).contains(QLatin1Char('/')));

		auto exact = store.analyze(catalog, &error);
		QCOMPARE(exact.update_kind, ImageryCatalogAnalysis::UpdateKind::ExactReimport);
		QCOMPARE(exact.added, 0);
		QCOMPARE(exact.changed, 0);
		QCOMPARE(exact.removed, 0);

		auto higher = ImageryCatalogReader::read(changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("revision"), 2); }));
		QCOMPARE(store.analyze(higher).update_kind, ImageryCatalogAnalysis::UpdateKind::HigherRevision);
		auto same_revision = ImageryCatalogReader::read(changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("description"), QStringLiteral("Republished")); }));
		QCOMPARE(store.analyze(same_revision).update_kind, ImageryCatalogAnalysis::UpdateKind::SameRevisionConflict);
		QVERIFY(store.install(higher, QStringLiteral("local"), {}, {}, &error));
		QCOMPARE(store.analyze(catalog).update_kind, ImageryCatalogAnalysis::UpdateKind::LowerRevision);

		auto duplicate = ImageryCatalogReader::read(changedCatalog([](QJsonObject& object) {
			object.insert(QStringLiteral("id"), QStringLiteral("org.example.imagery.duplicate-catalog"));
		}));
		QCOMPARE(store.analyze(duplicate).exact_duplicates, 1);
		QVERIFY(store.install(duplicate, QStringLiteral("local"), {}, {}, &error));

		auto potential = ImageryCatalogReader::read(changedCatalog([](QJsonObject& object) {
			object.insert(QStringLiteral("id"), QStringLiteral("org.example.imagery.potential-catalog"));
			auto sources = object.value(QStringLiteral("sources")).toArray();
			auto source = sources.first().toObject();
			source.insert(QStringLiteral("name"), QStringLiteral("Different description"));
			sources.replace(0, source);
			object.insert(QStringLiteral("sources"), sources);
		}));
		QCOMPARE(store.analyze(potential).potential_duplicates, 2);

		QFile generated(directory.filePath(QStringLiteral("generated-template.xml")));
		QVERIFY(generated.open(QIODevice::WriteOnly));
		generated.write("keep");
		generated.close();
		QVERIFY(store.remove(catalog.catalog.id, &error));
		QVERIFY(generated.exists());
		QCOMPARE(store.catalogs().size(), 1);
	}

	void catalogStoreWriteFailure()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const root_file = directory.filePath(QStringLiteral("not-a-directory"));
		QFile file(root_file);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("sentinel");
		file.close();
		ImageryCatalogStore store(root_file);
		auto const catalog = ImageryCatalogReader::read(fixture(QStringLiteral("valid/minimal.oic")));
		QString error;
		QVERIFY(!store.install(catalog, QStringLiteral("local"), {}, {}, &error));
		QVERIFY(!error.isEmpty());
		QVERIFY(file.open(QIODevice::ReadOnly));
		QCOMPARE(file.readAll(), QByteArray("sentinel"));
	}

	void invalidFixtures()
	{
		for (auto const& name : {
		       QStringLiteral("duplicate-member.oic"),
		       QStringLiteral("invalid-tile-url.oic"),
		       QStringLiteral("singular-affine.oic") })
		{
			auto const bytes = fixture(QStringLiteral("invalid/") + name);
			QVERIFY2(!bytes.isEmpty(), qPrintable(name));
			QVERIFY2(!ImageryCatalogReader::read(bytes).accepted(), qPrintable(name));
		}
	}

	void catalogErrors_data()
	{
		QTest::addColumn<QByteArray>("input");
		QTest::newRow("wrong-format") << changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("format"), QStringLiteral("example")); });
		QTest::newRow("future-version") << changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("version"), 2); });
		QTest::newRow("zero-revision") << changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("revision"), 0); });
		QTest::newRow("unknown-field") << changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("mystery"), true); });
		QTest::newRow("unnamespaced-extension") << changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("extensions"), QJsonObject { { QStringLiteral("local"), true } }); });
		QTest::newRow("unsupported-capability") << changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("requires"), QJsonArray { QStringLiteral("future.catalog.v1") }); });
		QTest::newRow("empty-sources") << changedCatalog([](QJsonObject& object) { object.insert(QStringLiteral("sources"), QJsonArray {}); });
	}

	void catalogErrors()
	{
		QFETCH(QByteArray, input);
		auto const result = ImageryCatalogReader::read(input);
		QVERIFY(!result.accepted());
		QVERIFY(hasIssue(result, ImageryCatalogIssue::Type::CatalogError));
	}

	void parserGuardrails_data()
	{
		QTest::addColumn<QByteArray>("input");
		QTest::newRow("duplicate-member") << QByteArray(R"({"format":"org.openorienteering.imagery-catalog","format":"org.openorienteering.imagery-catalog"})");
		QTest::newRow("escaped-duplicate-member") << QByteArray(R"({"id":"one","\u0069d":"two"})");
		QTest::newRow("invalid-utf8") << QByteArray("{\"x\":\"") + QByteArray(1, char(0xc0)) + QByteArray("\"}");
		QTest::newRow("unpaired-surrogate") << QByteArray(R"({"x":"\ud800"})");
		QTest::newRow("nonfinite-number") << QByteArray(R"({"x":1e400})");
		QByteArray nested;
		for (int i = 0; i < ImageryCatalogReader::max_nesting_depth + 1; ++i) nested += '[';
		nested += '0';
		for (int i = 0; i < ImageryCatalogReader::max_nesting_depth + 1; ++i) nested += ']';
		QTest::newRow("nesting") << nested;
	}

	void parserGuardrails()
	{
		QFETCH(QByteArray, input);
		auto const result = ImageryCatalogReader::read(input);
		QVERIFY(!result.accepted());
		QVERIFY(hasIssue(result, ImageryCatalogIssue::Type::CatalogError));
	}

	void documentSizeLimit()
	{
		QByteArray oversized(ImageryCatalogReader::max_document_size + 1, ' ');
		auto const result = ImageryCatalogReader::read(oversized);
		QVERIFY(!result.accepted());
		QVERIFY(hasIssue(result, ImageryCatalogIssue::Type::CatalogError, QStringLiteral("exceeds")));
	}

	void sourceErrors_data()
	{
		QTest::addColumn<QByteArray>("input");
		QTest::newRow("unknown-field") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("unknown"), true); });
		QTest::newRow("deferred-checksum-field") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("emptyTileChecksums"), QJsonArray {}); });
		QTest::newRow("file-url") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("tiles"), QJsonArray { QStringLiteral("file:///tmp/{z}/{x}/{y}") }); });
		QTest::newRow("userinfo") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("tiles"), QJsonArray { QStringLiteral("https://user:secret@example.test/{z}/{x}/{y}") }); });
		QTest::newRow("missing-placeholder") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("tiles"), QJsonArray { QStringLiteral("https://example.test/{z}/{x}/tile.png") }); });
		QTest::newRow("unknown-placeholder") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("tiles"), QJsonArray { QStringLiteral("https://example.test/{z}/{x}/{y}/{s}.png") }); });
		QTest::newRow("both-matrix-forms") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("tileMatrixSet"), ImageryCatalogReader::webMercatorQuad().original_object); });
		QTest::newRow("invalid-category") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("category"), QStringLiteral("photo")); });
		QTest::newRow("date-order") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("startDate"), QStringLiteral("2026-02-02")); source.insert(QStringLiteral("endDate"), QStringLiteral("2026-01-01")); });
		QTest::newRow("header-injection") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("request"), QJsonObject { { QStringLiteral("referer"), QStringLiteral("https://example.test/\r\nX-Test: yes") } }); });
		QTest::newRow("arbitrary-header") << changedSource([](QJsonObject& source) { source.insert(QStringLiteral("request"), QJsonObject { { QStringLiteral("headers"), QJsonObject { { QStringLiteral("Cookie"), QStringLiteral("secret") } } } }); });
	}

	void sourceErrors()
	{
		QFETCH(QByteArray, input);
		auto const result = ImageryCatalogReader::read(input);
		QVERIFY(!result.accepted());
		QVERIFY(!result.hasCatalogErrors());
		QVERIFY(hasIssue(result, ImageryCatalogIssue::Type::SourceError));
		QVERIFY(result.catalog.sources.isEmpty());
	}

	void invalidSourceDoesNotDiscardValidSource()
	{
		auto object = minimalCatalogObject();
		auto sources = object.value(QStringLiteral("sources")).toArray();
		auto invalid = sources.first().toObject();
		invalid.insert(QStringLiteral("id"), QStringLiteral("invalid"));
		invalid.insert(QStringLiteral("tiles"), QJsonArray { QStringLiteral("file:///invalid/{z}/{x}/{y}") });
		sources.push_back(invalid);
		object.insert(QStringLiteral("sources"), sources);

		auto const result = ImageryCatalogReader::read(json(object));
		QVERIFY(result.accepted());
		QCOMPARE(result.catalog.sources.size(), 1);
		QVERIFY(hasIssue(result, ImageryCatalogIssue::Type::SourceError));
	}

	void duplicateSourceIdIsCatalogError()
	{
		auto object = minimalCatalogObject();
		auto sources = object.value(QStringLiteral("sources")).toArray();
		sources.push_back(sources.first());
		object.insert(QStringLiteral("sources"), sources);
		auto const result = ImageryCatalogReader::read(json(object));
		QVERIFY(result.hasCatalogErrors());
		QVERIFY(hasIssue(result, ImageryCatalogIssue::Type::CatalogError, QStringLiteral("Duplicate source ID")));
	}

	void sourceLimits()
	{
		auto too_many_tiles = changedSource([](QJsonObject& source) {
			QJsonArray tiles;
			for (int i = 0; i < ImageryCatalogReader::max_tiles_per_source + 1; ++i)
				tiles.push_back(QStringLiteral("https://%1.example.test/{z}/{x}/{y}").arg(i));
			source.insert(QStringLiteral("tiles"), tiles);
		});
		QVERIFY(!ImageryCatalogReader::read(too_many_tiles).accepted());

		auto object = minimalCatalogObject();
		auto original = object.value(QStringLiteral("sources")).toArray().first().toObject();
		QJsonArray sources;
		for (int i = 0; i < ImageryCatalogReader::max_sources + 1; ++i)
		{
			auto source = original;
			source.insert(QStringLiteral("id"), QStringLiteral("source-%1").arg(i));
			sources.push_back(source);
		}
		object.insert(QStringLiteral("sources"), sources);
		auto const result = ImageryCatalogReader::read(json(object));
		QVERIFY(result.hasCatalogErrors());

		auto long_string = changedCatalog([](QJsonObject& catalog) {
			catalog.insert(QStringLiteral("description"), QString(ImageryCatalogReader::max_string_length + 1, QLatin1Char('x')));
		});
		QVERIFY(ImageryCatalogReader::read(long_string).hasCatalogErrors());

		auto long_url = changedSource([](QJsonObject& source) {
			QString const url = QStringLiteral("https://example.test/")
			                    + QString(ImageryCatalogReader::max_url_length, QLatin1Char('x'))
			                    + QStringLiteral("/{z}/{x}/{y}");
			source.insert(QStringLiteral("tiles"), QJsonArray {
				url
			});
		});
		QVERIFY(!ImageryCatalogReader::read(long_url).accepted());
	}

	void capabilities()
	{
		auto unknown = ImageryCatalogReader::read(changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("requires"), QJsonArray { QStringLiteral("org.example.future-renderer") });
		}));
		QVERIFY(unknown.accepted());
		QVERIFY(!unknown.catalog.sources.first().supported);
		QVERIFY(hasIssue(unknown, ImageryCatalogIssue::Type::UnsupportedSource));

		auto affine = ImageryCatalogReader::read(changedSource([](QJsonObject& source) {
			auto registration = translationRegistration();
			registration.insert(QStringLiteral("operation"), QJsonObject {
				{ QStringLiteral("type"), QStringLiteral("affine2d") }, { QStringLiteral("unit"), QStringLiteral("crs") },
				{ QStringLiteral("xoff"), 1 }, { QStringLiteral("yoff"), 2 },
				{ QStringLiteral("s11"), 1 }, { QStringLiteral("s12"), 0 },
				{ QStringLiteral("s21"), 0 }, { QStringLiteral("s22"), 1 }
			});
			source.insert(QStringLiteral("registration"), registration);
		}));
		QVERIFY(affine.accepted());
		QVERIFY(!affine.catalog.sources.first().supported);
		QVERIFY(hasIssue(affine, ImageryCatalogIssue::Type::UnsupportedSource, QStringLiteral("affine")));
	}

	void invalidRegistration_data()
	{
		QTest::addColumn<QByteArray>("input");
		QTest::newRow("direction") << changedSource([](QJsonObject& source) { auto registration = translationRegistration(); registration.insert(QStringLiteral("direction"), QStringLiteral("corrected-to-source")); source.insert(QStringLiteral("registration"), registration); });
		QTest::newRow("unit") << changedSource([](QJsonObject& source) { auto registration = translationRegistration(); auto operation = registration.value(QStringLiteral("operation")).toObject(); operation.insert(QStringLiteral("unit"), QStringLiteral("metre")); registration.insert(QStringLiteral("operation"), operation); source.insert(QStringLiteral("registration"), registration); });
		QTest::newRow("source-crs") << changedSource([](QJsonObject& source) { auto registration = translationRegistration(); registration.insert(QStringLiteral("sourceFrame"), QJsonObject { { QStringLiteral("crs"), QStringLiteral("EPSG:2927") } }); source.insert(QStringLiteral("registration"), registration); });
		QTest::newRow("singular-affine") << changedSource([](QJsonObject& source) {
			auto registration = translationRegistration();
			registration.insert(QStringLiteral("operation"), QJsonObject {
				{ QStringLiteral("type"), QStringLiteral("affine2d") }, { QStringLiteral("unit"), QStringLiteral("crs") },
				{ QStringLiteral("xoff"), 0 }, { QStringLiteral("yoff"), 0 },
				{ QStringLiteral("s11"), 1 }, { QStringLiteral("s12"), 2 },
				{ QStringLiteral("s21"), 2 }, { QStringLiteral("s22"), 4 }
			});
			source.insert(QStringLiteral("registration"), registration);
		});
		QTest::newRow("undeclared-grid") << changedSource([](QJsonObject& source) {
			auto registration = translationRegistration();
			registration.insert(QStringLiteral("operation"), QJsonObject {
				{ QStringLiteral("type"), QStringLiteral("gridShift") }, { QStringLiteral("resource"), QStringLiteral("missing-grid") },
				{ QStringLiteral("domain"), QStringLiteral("horizontal") },
				{ QStringLiteral("gridFrame"), QJsonObject { { QStringLiteral("crs"), QStringLiteral("EPSG:3857") } } },
				{ QStringLiteral("interpolation"), QStringLiteral("bilinear") }
			});
			source.insert(QStringLiteral("registration"), registration);
		});
	}

	void invalidRegistration()
	{
		QFETCH(QByteArray, input);
		auto const result = ImageryCatalogReader::read(input);
		QVERIFY(!result.accepted());
		QVERIFY(hasIssue(result, ImageryCatalogIssue::Type::SourceError));
	}

	void matrixValidation()
	{
		auto out_of_bounds = fixture(QStringLiteral("valid/custom-dyadic-epsg2927.oic"));
		auto object = QJsonDocument::fromJson(out_of_bounds).object();
		auto sources = object.value(QStringLiteral("sources")).toArray();
		auto source = sources.first().toObject();
		auto limits = source.value(QStringLiteral("tileMatrixLimits")).toArray();
		auto limit = limits.first().toObject();
		limit.insert(QStringLiteral("maxTileCol"), 32);
		limits.replace(0, limit);
		source.insert(QStringLiteral("tileMatrixLimits"), limits);
		sources.replace(0, source);
		object.insert(QStringLiteral("sources"), sources);
		auto const invalid = ImageryCatalogReader::read(json(object));
		QVERIFY(!invalid.accepted());
		QVERIFY(hasIssue(invalid, ImageryCatalogIssue::Type::SourceError, QStringLiteral("outside")));

		auto nondyadic = changedSource([](QJsonObject& changed) {
			auto matrix_set = ImageryCatalogReader::webMercatorQuad().original_object;
			auto matrices = matrix_set.value(QStringLiteral("tileMatrices")).toArray();
			matrices.removeLast();
			matrices.removeLast();
			auto second = matrices.at(1).toObject();
			second.insert(QStringLiteral("cellSize"), second.value(QStringLiteral("cellSize")).toDouble() * 1.1);
			matrices.replace(1, second);
			matrix_set.insert(QStringLiteral("tileMatrices"), matrices);
			changed.remove(QStringLiteral("tileMatrixSetURI"));
			changed.insert(QStringLiteral("tileMatrixSet"), matrix_set);
		});
		auto const unsupported = ImageryCatalogReader::read(nondyadic);
		QVERIFY(unsupported.accepted());
		QVERIFY(!unsupported.catalog.sources.first().supported);
	}

	void resourcesAndCoverageLimits()
	{
		auto invalid_resource = changedCatalog([](QJsonObject& catalog) {
			catalog.insert(QStringLiteral("resources"), QJsonObject {
				{ QStringLiteral("grid"), QJsonObject {
					{ QStringLiteral("href"), QStringLiteral("http://example.test/grid.tif") },
					{ QStringLiteral("mediaType"), QStringLiteral("image/tiff") },
					{ QStringLiteral("sha256"), QStringLiteral("bad") },
					{ QStringLiteral("size"), 100 }
				} }
			});
		});
		QVERIFY(ImageryCatalogReader::read(invalid_resource).hasCatalogErrors());

		auto too_many_vertices = changedSource([](QJsonObject& source) {
			QJsonArray ring;
			for (int i = 0; i < ImageryCatalogReader::max_coverage_vertices + 1; ++i)
				ring.push_back(QJsonArray { -122.0, 47.0 });
			// A braced single same-type element may select the copy constructor
			// instead of initializer_list construction, dropping a nesting level.
			QJsonArray rings;
			rings.push_back(ring);
			source.insert(QStringLiteral("coverage"), QJsonObject {
				{ QStringLiteral("type"), QStringLiteral("Polygon") },
				{ QStringLiteral("coordinates"), rings }
			});
		});
		auto const result = ImageryCatalogReader::read(too_many_vertices);
		QVERIFY(!result.accepted());
		QVERIFY(hasIssue(result, ImageryCatalogIssue::Type::SourceError, QStringLiteral("vertex limit")));
	}
};

}  // namespace OpenOrienteering

using OpenOrienteering::ImageryCatalogTest;

QTEST_APPLESS_MAIN(ImageryCatalogTest)
#include "imagery_catalog_t.moc"
