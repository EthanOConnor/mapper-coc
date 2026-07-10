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

#include <memory>
#include <vector>

#include <QtTest>
#include <QBuffer>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QRadioButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "test_config.h"

#include "global.h"
#include "core/georeferencing.h"
#include "core/map.h"
#include "gdal/gdal_image_reader.h"
#include "gdal/gdal_manager.h"
#include "gdal/gdal_template.h"
#include "gdal/imagery_catalog_reader.h"
#include "gdal/imagery_catalog_store.h"
#include "gdal/imagery_source_resolver.h"
#include "gdal/online_imagery_template_builder.h"
#include "gui/widgets/online_template_dialog.h"
#include "templates/template.h"

namespace OpenOrienteering {

namespace {

QString createTiledGeoTiff(const QString& path,
                           const QSize& raster_size,
                           const QSize& block_size,
                           bool georeferenced)
{
	auto* driver = GDALGetDriverByName("GTiff");
	if (!driver)
		return {};

	char** options = nullptr;
	if (!block_size.isEmpty())
	{
		options = CSLSetNameValue(options, "TILED", "YES");
		options = CSLSetNameValue(options, "BLOCKXSIZE", QByteArray::number(block_size.width()).constData());
		options = CSLSetNameValue(options, "BLOCKYSIZE", QByteArray::number(block_size.height()).constData());
	}

	auto* dataset = GDALCreate(driver,
	                           path.toUtf8().constData(),
	                           raster_size.width(),
	                           raster_size.height(),
	                           1,
	                           GDT_Byte,
	                           options);
	CSLDestroy(options);
	if (!dataset)
		return {};

	std::vector<GByte> pixels(std::size_t(raster_size.width()) * raster_size.height());
	for (int y = 0; y < raster_size.height(); ++y)
	{
		for (int x = 0; x < raster_size.width(); ++x)
			pixels[std::size_t(y) * raster_size.width() + x] = GByte((x + y) % 256);
	}

	auto result = GDALRasterIO(GDALGetRasterBand(dataset, 1),
	                           GF_Write,
	                           0,
	                           0,
	                           raster_size.width(),
	                           raster_size.height(),
	                           pixels.data(),
	                           raster_size.width(),
	                           raster_size.height(),
	                           GDT_Byte,
	                           0,
	                           0);
	if (result < CE_Warning && georeferenced)
	{
		double geo_transform[6] = { 410000.0, 2.0, 0.0, 5300000.0, 0.0, -2.0 };
		result = GDALSetGeoTransform(dataset, geo_transform);
		if (result < CE_Warning)
		{
			auto* srs = OSRNewSpatialReference(nullptr);
			if (srs && OSRSetFromUserInput(srs, "EPSG:3857") == OGRERR_NONE)
			{
				char* wkt = nullptr;
				if (OSRExportToWkt(srs, &wkt) == OGRERR_NONE)
				{
					result = GDALSetProjection(dataset, wkt);
					CPLFree(wkt);
				}
			}
			OSRDestroySpatialReference(srs);
		}
	}

	GDALClose(dataset);
	return (result < CE_Warning) ? path : QString{};
}

QString writeTextFile(const QString& path, const QByteArray& contents)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		return {};
	if (file.write(contents) != contents.size())
		return {};
	return path;
}

QString createTmsXml(const QString& path)
{
	return writeTextFile(
		path,
		R"(<?xml version="1.0" encoding="UTF-8"?>
<GDAL_WMS>
  <Service name="TMS">
    <ServerUrl>https://example.test/${z}/${x}/${y}.png</ServerUrl>
  </Service>
  <DataWindow>
    <UpperLeftX>-13618288.0</UpperLeftX>
    <UpperLeftY>6050654.0</UpperLeftY>
    <LowerRightX>-13617776.0</LowerRightX>
    <LowerRightY>6050142.0</LowerRightY>
    <SizeX>512</SizeX>
    <SizeY>512</SizeY>
    <TileLevel>18</TileLevel>
    <TileX>84192</TileX>
    <TileY>183072</TileY>
    <YOrigin>top</YOrigin>
  </DataWindow>
  <Projection>EPSG:3857</Projection>
  <BlockSizeX>256</BlockSizeX>
  <BlockSizeY>256</BlockSizeY>
  <BandsCount>3</BandsCount>
  <ZeroBlockHttpCodes>404</ZeroBlockHttpCodes>
  <Cache />
</GDAL_WMS>
)");
}

}  // namespace


class GdalTiledTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		QCoreApplication::setOrganizationName(QString::fromLatin1("OpenOrienteering.org"));
		QCoreApplication::setApplicationName(QStringLiteral("GdalTiledTest"));
		QVERIFY2(QDir::home().exists(), "The home dir must be writable in order to use QSettings.");

		doStaticInitializations();
		GdalManager();
		QDir::addSearchPath(QStringLiteral("testdata"),
		                    QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)).absoluteFilePath(QStringLiteral("data")));
	}

	void tiledRasterDetectionTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("tiled-64.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     true);
		QVERIFY(!tiled_path.isEmpty());
		auto tiled_info = GdalImageReader(tiled_path).readRasterInfo();
		QCOMPARE(tiled_info.size, QSize(128, 128));
		QCOMPARE(tiled_info.block_size, QSize(64, 64));

		auto small_tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("tiled-32.tif")),
		                                           QSize(128, 128),
		                                           QSize(32, 32),
		                                           true);
		QVERIFY(!small_tiled_path.isEmpty());
		auto small_tiled_info = GdalImageReader(small_tiled_path).readRasterInfo();
		QCOMPARE(small_tiled_info.block_size, QSize());
	}

	void tmsTileOriginParsingTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto const tms_xml_path = createTmsXml(dir.filePath(QStringLiteral("tms.xml")));
		QVERIFY(!tms_xml_path.isEmpty());

		QPoint origin_tile;
		QVERIFY(GdalTemplate::readTmsTileOrigin(tms_xml_path, &origin_tile));
		QCOMPARE(origin_tile, QPoint(84192, 183072));

		auto const wms_xml_path = writeTextFile(
			dir.filePath(QStringLiteral("wms.xml")),
			R"(<?xml version="1.0" encoding="UTF-8"?>
<GDAL_WMS>
  <Service name="WMS">
    <ServerUrl>https://example.test/wms</ServerUrl>
  </Service>
  <DataWindow>
    <TileX>10</TileX>
    <TileY>20</TileY>
  </DataWindow>
</GDAL_WMS>
)");
		QVERIFY(!wms_xml_path.isEmpty());
		QVERIFY(!GdalTemplate::readTmsTileOrigin(wms_xml_path, &origin_tile));
	}

	void onlineImageryClassificationTest()
	{
		auto arcgis_template = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"));
		QCOMPARE(arcgis_template.source.kind, OnlineImagerySource::Kind::ArcGisTiledMapServer);
		QCOMPARE(arcgis_template.source.normalized_url,
		         QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer"));

		auto xyz = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://tiles.example.test/{z}/{x}/{y}.png"));
		QCOMPARE(xyz.source.kind, OnlineImagerySource::Kind::XyzTiles);
		QCOMPARE(xyz.source.normalized_url,
		         QStringLiteral("https://tiles.example.test/${z}/${x}/${y}.png"));
		QCOMPARE(xyz.source.tile_size, QSize(256, 256));
		QCOMPARE(xyz.source.max_tile_level, 19);

		auto arcgis = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer/tile/5/10/12"));
		QCOMPARE(arcgis.source.kind, OnlineImagerySource::Kind::ArcGisTiledMapServer);
		QCOMPARE(arcgis.source.normalized_url,
		         QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer"));

		auto arcgis_root = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer"));
		QCOMPARE(arcgis_root.source.kind, OnlineImagerySource::Kind::ArcGisTiledMapServer);
		QCOMPARE(arcgis_root.source.normalized_url,
		         QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer"));
		QCOMPARE(arcgis_root.source.tile_size, QSize(256, 256));
		QCOMPARE(arcgis_root.source.max_tile_level, 20);

		auto unsupported = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://tiles.example.test/{s}/{z}/{x}/{y}.png"));
		QVERIFY(!unsupported.error.isEmpty());

		auto unrecognized = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://tiles.example.test/not-a-template"));
		QVERIFY(unrecognized.error.contains(QStringLiteral("top-origin Web Mercator XYZ")));
	}

	void onlineImageryChooserContainsOnlyRecentSourcesTest()
	{
		QSettings settings;
		settings.remove(QStringLiteral("onlineImagery"));

		Map map;
		OnlineTemplateDialog empty_dialog(map, QString{}, QRectF{});
		auto* empty_chooser = empty_dialog.findChild<QComboBox*>();
		QVERIFY(empty_chooser);
		QCOMPARE(empty_chooser->count(), 1);
		QCOMPARE(empty_chooser->itemText(0), QStringLiteral("Choose an imagery source"));

		settings.setValue(
			QStringLiteral("onlineImagery/recentUrls"),
			QStringList{QStringLiteral("https://tiles.example.test/{z}/{x}/{y}.png")});
		settings.setValue(
			QStringLiteral("onlineImagery/recentNames"),
			QStringList{QStringLiteral("Example imagery")});

		OnlineTemplateDialog recent_dialog(map, QString{}, QRectF{});
		auto* recent_chooser = recent_dialog.findChild<QComboBox*>();
		QVERIFY(recent_chooser);
		QCOMPARE(recent_chooser->count(), 3);
		QCOMPARE(recent_chooser->itemText(2), QStringLiteral("Example imagery"));

		settings.remove(QStringLiteral("onlineImagery"));
	}

	void onlineImageryManualSourcePlaceholderTest()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const classified = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://tiles.example.test/{z}/{x}/{y}.png"));
		QVERIFY(classified.error.isEmpty());
		Georeferencing georef;
		QVERIFY(georef.setProjectedCRS(QStringLiteral("Web Mercator"), QStringLiteral("EPSG:3857")));

		auto const generated = OnlineImageryTemplateBuilder::generateXml(
			classified.source,
			QStringLiteral("Manual imagery"),
			QRectF(-1000, -1000, 2000, 2000),
			georef,
			directory.filePath(QStringLiteral("manual.omap")));
		QVERIFY2(generated.error.isEmpty(), qPrintable(generated.error));
		QFile xml(generated.xml_path);
		QVERIFY(xml.open(QIODevice::ReadOnly));
		auto const contents = xml.readAll();
		QVERIFY(contents.contains("https://tiles.example.test/${z}/${x}/${y}.png"));
		QVERIFY(!contents.contains("$$"));
	}

	void onlineImageryCatalogImportTest()
	{
		QString const catalog_path = QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)
		                             + QStringLiteral("/data/imagery-catalogs/valid/minimal.oic");
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const store_root = directory.filePath(QStringLiteral("catalog-store"));
		ImageryCatalogStore store(store_root);
		QString error;
		Georeferencing georef;
		QVERIFY(georef.setProjectedCRS(QStringLiteral("Web Mercator"), QStringLiteral("EPSG:3857")));
		Map map;
		map.setGeoreferencing(georef);
		OnlineTemplateDialog dialog(
			map,
			directory.filePath(QStringLiteral("catalog-import.omap")),
			QRectF(-1000, -1000, 2000, 2000));
		QVERIFY(QMetaObject::invokeMethod(
			&dialog,
			"importCatalogFile",
			Qt::DirectConnection,
			Q_ARG(QString, catalog_path),
			Q_ARG(QString, store_root)));

		auto* chooser = dialog.findChild<QComboBox*>(QStringLiteral("source_chooser"));
		QVERIFY(chooser);
		QCOMPARE(chooser->count(), 3);
		QCOMPARE(chooser->itemText(2), QStringLiteral("Example aerial"));
		QCOMPARE(chooser->currentIndex(), 2);
		auto* url_edit = dialog.findChild<QLineEdit*>(QStringLiteral("imagery_url"));
		QVERIFY(url_edit);
		QCOMPARE(url_edit->text(), QStringLiteral("https://tiles.example.test/aerial/{z}/{x}/{y}.png"));

		auto installed = store.catalogs(&error);
		QCOMPARE(installed.size(), 1);
		QCOMPARE(installed.first().state.origin, QUrl::fromLocalFile(catalog_path).toString());
		auto const original_fingerprint = installed.first().read_result.catalog.sources.first().operational_fingerprint;

		QFile catalog_file(catalog_path);
		QVERIFY(catalog_file.open(QIODevice::ReadOnly));
		auto catalog_object = QJsonDocument::fromJson(catalog_file.readAll()).object();
		catalog_object.insert(QStringLiteral("revision"), 2);
		auto sources = catalog_object.value(QStringLiteral("sources")).toArray();
		auto changed_source = sources.first().toObject();
		changed_source.insert(QStringLiteral("name"), QStringLiteral("Updated aerial"));
		changed_source.insert(
			QStringLiteral("tiles"),
			QJsonArray { QStringLiteral("https://tiles.example.test/updated/{z}/{x}/{y}.png") });
		sources.replace(0, changed_source);
		catalog_object.insert(QStringLiteral("sources"), sources);
		auto const changed_bytes = QJsonDocument(catalog_object).toJson(QJsonDocument::Indented);
		auto const changed_path = writeTextFile(
			directory.filePath(QStringLiteral("changed-catalog.oic")),
			changed_bytes);
		QVERIFY(!changed_path.isEmpty());
		auto const changed_catalog = ImageryCatalogReader::read(changed_bytes);
		QVERIFY(changed_catalog.accepted());
		auto const changed_fingerprint = changed_catalog.catalog.sources.first().operational_fingerprint;
		QVERIFY(changed_fingerprint != original_fingerprint);

		QVERIFY(QMetaObject::invokeMethod(
			&dialog,
			"importCatalogFile",
			Qt::DirectConnection,
			Q_ARG(QString, changed_path),
			Q_ARG(QString, store_root)));
		QCOMPARE(chooser->count(), 3);
		QCOMPARE(chooser->itemText(2), QStringLiteral("Updated aerial"));
		QCOMPARE(chooser->currentIndex(), 2);
		QCOMPARE(url_edit->text(), QStringLiteral("https://tiles.example.test/updated/{z}/{x}/{y}.png"));

		installed = store.catalogs(&error);
		QCOMPARE(installed.size(), 1);
		QCOMPARE(installed.first().read_result.catalog.sources.first().operational_fingerprint,
		         changed_fingerprint);

		auto* current_view = dialog.findChild<QRadioButton*>(QStringLiteral("current_view_coverage"));
		QVERIFY(current_view);
		current_view->setChecked(true);
		QVERIFY(QMetaObject::invokeMethod(&dialog, "onAddClicked", Qt::DirectConnection));
		QCOMPARE(dialog.result(), int(QDialog::Accepted));
		QFile generated(dialog.generatedPath());
		QVERIFY(generated.open(QIODevice::ReadOnly));
		auto const generated_xml = generated.readAll();
		QVERIFY(generated_xml.contains("https://tiles.example.test/updated/${z}/${x}/${y}.png"));
		QVERIFY(generated_xml.contains(changed_fingerprint));
		QVERIFY(!generated_xml.contains(original_fingerprint));
		QVERIFY(generated_xml.contains("<ZeroBlockHttpCodes>404</ZeroBlockHttpCodes>"));
		QPoint generated_origin;
		QVERIFY(GdalTemplate::readTmsTileOrigin(dialog.generatedPath(), &generated_origin));
		QCOMPARE(generated_origin.x() % 64, 0);
		QCOMPARE(generated_origin.y() % 64, 0);

		QVERIFY2(store.remove(QStringLiteral("org.example.imagery.minimal"), &error), qPrintable(error));
	}

	void onlineImageryCoordinateMathTest()
	{
		auto const origin = OnlineImageryTemplateBuilder::latLonToWebMercator(0.0, 0.0);
		QVERIFY(qAbs(origin.x()) < 0.001);
		QVERIFY(qAbs(origin.y()) < 0.001);

		auto const east = OnlineImageryTemplateBuilder::latLonToWebMercator(0.0, 180.0);
		QVERIFY(qAbs(east.x() - 20037508.342789244) < 0.001);
		QVERIFY(qAbs(east.y()) < 0.001);

		auto const crop = OnlineImageryTemplateBuilder::snapToTileGrid(
			QRectF(-10018754.171394622, -10018754.171394622, 20037508.342789244, 20037508.342789244),
			2,
			256);
		QCOMPARE(crop.tile_x_min, 0);
		QCOMPARE(crop.tile_x_max, 3);
		QCOMPARE(crop.tile_y_min, 0);
		QCOMPARE(crop.tile_y_max, 3);
		QCOMPARE(crop.pixel_width, 1024);
		QCOMPARE(crop.pixel_height, 1024);
		QVERIFY(qAbs(crop.west + 20037508.342789244) < 0.001);
		QVERIFY(qAbs(crop.north - 20037508.342789244) < 0.001);
		QVERIFY(qAbs(crop.east - 20037508.342789244) < 0.001);
		QVERIFY(qAbs(crop.south + 20037508.342789244) < 0.001);

		auto const detailed_crop = OnlineImageryTemplateBuilder::snapToTileGrid(
			QRectF(-13602122.057404, 6039136.730755, 4891.969811, 4891.969810),
			19,
			256);
		QCOMPARE(detailed_crop.tile_x_min % 32, 0);
		QCOMPARE(detailed_crop.tile_y_min % 32, 0);
		QCOMPARE(detailed_crop.tile_x_max % 32, 31);
		QCOMPARE(detailed_crop.tile_y_max % 32, 31);
	}

	void onlineImageryOutputFileNameTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		OnlineImagerySource source;
		source.kind = OnlineImagerySource::Kind::XyzTiles;
		source.display_name = QStringLiteral("example imagery");
		source.normalized_url = QStringLiteral("https://tiles.example.test/${z}/${x}/${y}.png");

		auto named_path = OnlineImageryTemplateBuilder::outputFileName(
			dir.filePath(QStringLiteral("wilburton.omap")),
			source,
			QStringLiteral("Wilburton Hill"));
		QCOMPARE(QFileInfo(named_path).dir().absolutePath(), dir.path());
		QVERIFY(QFileInfo(named_path).fileName().startsWith(QStringLiteral("wilburton_hill_wilburton_online_")));
		QVERIFY(named_path.endsWith(QStringLiteral(".xml")));

		auto default_path = OnlineImageryTemplateBuilder::outputFileName(
			dir.filePath(QStringLiteral("wilburton.omap")),
			source,
			QString{});
		QVERIFY(QFileInfo(default_path).fileName().startsWith(QStringLiteral("example_imagery_wilburton_online_")));

		source.operational_fingerprint = QByteArray(64, 'a');
		auto fingerprint_path = OnlineImageryTemplateBuilder::outputFileName(
			dir.filePath(QStringLiteral("wilburton.omap")), source, QStringLiteral("Catalog source"));
		QVERIFY(fingerprint_path.contains(QStringLiteral("aaaaaaaaaaaa.xml")));
		QVERIFY(!writeTextFile(fingerprint_path, QByteArray("unrelated file")).isEmpty());
		auto extended_path = OnlineImageryTemplateBuilder::outputFileName(
			dir.filePath(QStringLiteral("wilburton.omap")), source, QStringLiteral("Catalog source"));
		QVERIFY(extended_path.contains(QStringLiteral("aaaaaaaaaaaaaaaa.xml")));
	}

	void customImageryTileGridTest()
	{
		QFile fixture(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)
		              + QStringLiteral("/data/imagery-catalogs/valid/custom-dyadic-epsg2927.oic"));
		QVERIFY(fixture.open(QIODevice::ReadOnly));
		auto const catalog = ImageryCatalogReader::read(fixture.readAll());
		QVERIFY(catalog.accepted());
		auto const resolved = ImagerySourceResolver::resolve(catalog.catalog.sources.first());
		QVERIFY(resolved.error.isEmpty());
		auto const& matrix = resolved.source.tile_matrix_set.tile_matrices.last();
		auto const span = matrix.cell_size * matrix.tile_size.width();
		auto const bbox = QRectF(matrix.point_of_origin.x() + 2.1 * span,
		                         matrix.point_of_origin.y() - 3.9 * span,
		                         1.7 * span,
		                         1.7 * span);
		auto const crop = OnlineImageryTemplateBuilder::snapToTileGrid(bbox, resolved.source);
		QCOMPARE(crop.tile_level, 2);
		QCOMPARE(crop.tile_x_min, 0);
		QCOMPARE(crop.tile_x_max, 3);
		QCOMPARE(crop.tile_y_min, 0);
		QCOMPARE(crop.tile_y_max, 3);
		QCOMPARE(crop.tile_x_min % 64, 0);
		QCOMPARE(crop.tile_y_min % 64, 0);
		QCOMPARE(crop.pixel_width, 1024);
		QCOMPARE(crop.pixel_height, 1024);

		Map map;
		GdalTemplate generated(QStringLiteral("catalog.xml"), &map);
		generated.tiled_raster_info.block_size = matrix.tile_size;
		generated.has_tiled_origin_tile = true;
		generated.tiled_origin_tile = QPoint(crop.tile_x_min, crop.tile_y_min);
		QCOMPARE(generated.chooseTiledSubsampling(0.02), 64);
	}

	void tiledCoreMathTest()
	{
		auto const normalized = GdalTemplate::tileKey(3, 4, 0);
		QCOMPARE(normalized.tile_x, 3);
		QCOMPARE(normalized.tile_y, 4);
		QCOMPARE(normalized.subsampling, 1);

		QSet<GdalTileKey> keys;
		keys.insert(normalized);
		QVERIFY(keys.contains(GdalTemplate::tileKey(3, 4, 1)));

		QCOMPARE(GdalTemplate::chooseTileSubsampling(4.0, QSize(64, 64)), 1);
		QCOMPARE(GdalTemplate::chooseTileSubsampling(0.75, QSize(64, 64)), 1);
		QCOMPARE(GdalTemplate::chooseTileSubsampling(0.5, QSize(64, 64)), 2);
		QCOMPARE(GdalTemplate::chooseTileSubsampling(0.25, QSize(64, 64)), 4);

		QCOMPARE(GdalTemplate::sourceRectForTile(QSize(130, 130), QSize(64, 64), 0, 0, 1),
		         QRect(0, 0, 64, 64));
		QCOMPARE(GdalTemplate::sourceRectForTile(QSize(130, 130), QSize(64, 64), 2, 2, 1),
		         QRect(128, 128, 2, 2));
		QCOMPARE(GdalTemplate::sourceRectForTile(QSize(130, 130), QSize(64, 64), 0, 0, 2),
		         QRect(0, 0, 128, 128));
		QCOMPARE(GdalTemplate::sourceRectForTile(QSize(130, 130), QSize(64, 64), 2, 2, 2),
		         QRect());
		QCOMPARE(GdalTemplate::sourceRectWithinCachedTile(QRect(64, 0, 64, 64),
		                                                  QRect(0, 0, 128, 128),
		                                                  QSize(64, 64)),
		         QRectF(32.0, 0.0, 32.0, 32.0));
		QCOMPARE(GdalTemplate::sourceRectWithinCachedTile(QRect(128, 0, 2, 64),
		                                                  QRect(0, 0, 130, 128),
		                                                  QSize(65, 64)),
		         QRectF(64.0, 0.0, 1.0, 32.0));

		Map map;
		GdalTemplate temp(QStringLiteral("dummy.tif"), &map);
		temp.tiled_dataset = reinterpret_cast<GDALDatasetH>(quintptr(1));
		temp.tiled_raster_size = QSize(256, 128);
		temp.tiled_raster_info.block_size = QSize(64, 64);

		auto const full_window = temp.tileWindowForMapRect(QRectF(-128.0, -64.0, 256.0, 128.0), 1);
		QCOMPARE(full_window.tile_x_min, 0);
		QCOMPARE(full_window.tile_y_min, 0);
		QCOMPARE(full_window.tile_x_max, 3);
		QCOMPARE(full_window.tile_y_max, 1);
		QCOMPARE(full_window.subsampling, 1);

		auto const edge_window = temp.tileWindowForMapRect(QRectF(96.0, -64.0, 64.0, 128.0), 2);
		QCOMPARE(edge_window.tile_x_min, 1);
		QCOMPARE(edge_window.tile_y_min, 0);
		QCOMPARE(edge_window.tile_x_max, 1);
		QCOMPARE(edge_window.tile_y_max, 0);
		QCOMPARE(edge_window.subsampling, 2);

		QCOMPARE(temp.chooseTiledSubsampling(4.0), 1);
		temp.has_tiled_origin_tile = true;
		temp.tiled_origin_tile = QPoint(6, 12);
		QCOMPARE(temp.chooseTiledSubsampling(0.125), 2);
		temp.tiled_origin_tile = QPoint(84192, 183072);
		QCOMPARE(temp.chooseTiledSubsampling(0.03), 32);
		QCOMPARE(temp.chooseTiledSubsampling(0.01), 32);

		auto const coarse_key = GdalTemplate::tileKey(0, 0, 2);
		temp.tile_cache.insert(coarse_key, GdalTemplate::CachedTileEntry{ QImage(64, 64, QImage::Format_ARGB32_Premultiplied) });

		QRectF fallback_source_rect;
		auto const* fallback = temp.findBestCachedTile(1, 0, 1, &fallback_source_rect);
		QVERIFY(fallback);
		QCOMPARE(fallback->size(), QSize(64, 64));
		QCOMPARE(fallback_source_rect, QRectF(32.0, 0.0, 32.0, 32.0));

		temp.tiled_dataset = nullptr;
	}

	void workerCountForSourceTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("local.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     true);
		QVERIFY(!tiled_path.isEmpty());

		Map map;

		GdalTemplate local_template(tiled_path, &map);
		local_template.tiled_dataset = GDALOpen(tiled_path.toUtf8(), GA_ReadOnly);
		QVERIFY(local_template.tiled_dataset);
		QCOMPARE(local_template.workerCountForSource(), 1);
		QCOMPARE(GdalTemplate::workerCountForDriverName("GTiff"), 1);
		QCOMPARE(GdalTemplate::workerCountForDriverName("WMS"), 4);
		QCOMPARE(GdalTemplate::workerCountForDriverName(nullptr), 1);
		local_template.shutdownTiledSource();
	}

	void staleTileCompletionIgnoredAfterShutdownTest()
	{
		Map map;
		GdalTemplate temp(QStringLiteral("dummy.tif"), &map);
		auto const key = GdalTemplate::tileKey(0, 0, 1);
		auto const stale_generation = temp.tile_generation.load();

		temp.shutdownTiledSource();
		temp.onTileLoaded(key, QImage(8, 8, QImage::Format_ARGB32_Premultiplied), stale_generation);
		QVERIFY(temp.tile_cache.isEmpty());
		QCOMPARE(temp.tile_cache_bytes, qsizetype(0));

		temp.loading_tiles.insert(key);
		temp.onTileLoadFailed(key, stale_generation);
		QVERIFY(temp.loading_tiles.contains(key));
		temp.onTileLoadFailed(key, temp.tile_generation.load());
		QVERIFY(!temp.loading_tiles.contains(key));

		temp.onTileLoaded(
			key,
			QImage(8, 8, QImage::Format_ARGB32_Premultiplied),
			temp.tile_generation.load());
		QVERIFY(temp.tile_cache.isEmpty());
		QCOMPARE(temp.tile_cache_bytes, qsizetype(0));
	}

	void duplicateLoadedTiledTemplateTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("roundtrip.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     true);
		QVERIFY(!tiled_path.isEmpty());

		Map map;
		auto temp = Template::templateForPath(tiled_path, &map);
		QVERIFY(temp);
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");

		map.addTemplate(0, std::move(temp));
		auto* gdal_template = dynamic_cast<GdalTemplate*>(map.getTemplate(0));
		QVERIFY(gdal_template);
		gdal_template->setTemplateState(Template::Unloaded);
		QVERIFY(gdal_template->loadTemplateFile());
		QCOMPARE(gdal_template->getTemplateState(), Template::Loaded);
		QVERIFY(gdal_template->isTiledSource());
		QVERIFY(gdal_template->isTemplateGeoreferenced());
		QCOMPARE(gdal_template->workerCountForSource(), 1);
		QCOMPARE(gdal_template->worker_datasets.size(), std::size_t(1));
		QCOMPARE(gdal_template->worker_threads.size(), std::size_t(1));

		std::unique_ptr<Template> duplicate{gdal_template->duplicate()};
		QVERIFY(duplicate);
		QCOMPARE(duplicate->getTemplateType(), "GdalTemplate");
		QCOMPARE(duplicate->getTemplateState(), Template::Loaded);
		auto* duplicate_gdal = dynamic_cast<GdalTemplate*>(duplicate.get());
		QVERIFY(duplicate_gdal);
		QVERIFY(duplicate_gdal->isTiledSource());
		QVERIFY(duplicate_gdal->isTemplateGeoreferenced());
		QCOMPARE(duplicate_gdal->workerCountForSource(), 1);
		QCOMPARE(duplicate_gdal->worker_datasets.size(), std::size_t(1));
		QCOMPARE(duplicate_gdal->worker_threads.size(), std::size_t(1));
	}

	void roundTripGeoreferencedTiledTemplateTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("roundtrip.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     true);
		QVERIFY(!tiled_path.isEmpty());

		Map map;
		auto temp = Template::templateForPath(tiled_path, &map);
		QVERIFY(temp);
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");

		map.addTemplate(0, std::move(temp));
		auto* gdal_template = dynamic_cast<GdalTemplate*>(map.getTemplate(0));
		QVERIFY(gdal_template);
		gdal_template->setTemplateState(Template::Unloaded);
		QVERIFY(gdal_template->loadTemplateFile());
		QCOMPARE(gdal_template->getTemplateState(), Template::Loaded);
		QVERIFY(gdal_template->isTiledSource());
		QVERIFY(gdal_template->isTemplateGeoreferenced());

		auto const original_crs = gdal_template->availableGeoreferencing().effective.crs_spec;
		QVERIFY(!original_crs.isEmpty());

		gdal_template->unloadTemplateFile();
		QCOMPARE(gdal_template->getTemplateState(), Template::Unloaded);

		QBuffer buffer;
		QVERIFY(buffer.open(QIODevice::ReadWrite));
		QXmlStreamWriter writer(&buffer);
		gdal_template->saveTemplateConfiguration(writer, true, nullptr);
		QVERIFY(buffer.seek(0));

		QXmlStreamReader reader(&buffer);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		Map reloaded_map;
		auto reloaded_template_holder = Template::loadTemplateConfiguration(reader, reloaded_map, open);
		QVERIFY(reloaded_template_holder);
		QVERIFY(open);

		auto* reloaded_gdal = dynamic_cast<GdalTemplate*>(reloaded_template_holder.get());
		QVERIFY(reloaded_gdal);
		QCOMPARE(reloaded_gdal->getTemplateType(), "GdalTemplate");
		QVERIFY(reloaded_gdal->isTemplateGeoreferenced());
		QCOMPARE(reloaded_gdal->getTemplateFilename(), QStringLiteral("roundtrip.tif"));
		QVERIFY(reloaded_gdal->loadTemplateFile());
		QCOMPARE(reloaded_gdal->getTemplateState(), Template::Loaded);
		QVERIFY(reloaded_gdal->isTiledSource());
		QCOMPARE(reloaded_gdal->availableGeoreferencing().effective.crs_spec, original_crs);
	}

	void nonGeoreferencedTiledRasterFallsBackToFullImageTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("non-georef.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     false);
		QVERIFY(!tiled_path.isEmpty());

		Map map;
		auto temp = Template::templateForPath(tiled_path, &map);
		QVERIFY(temp);
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");

		map.addTemplate(0, std::move(temp));
		auto* gdal_template = dynamic_cast<GdalTemplate*>(map.getTemplate(0));
		QVERIFY(gdal_template);
		gdal_template->setTemplateState(Template::Unloaded);
		QVERIFY(gdal_template->loadTemplateFile());
		QCOMPARE(gdal_template->getTemplateState(), Template::Loaded);
		QVERIFY(!gdal_template->isTiledSource());
		QVERIFY(!gdal_template->isTemplateGeoreferenced());
		QCOMPARE(gdal_template->getTemplateExtent(), QRectF(-64.0, -64.0, 128.0, 128.0));
	}
};

}  // namespace OpenOrienteering

using OpenOrienteering::GdalTiledTest;

QTEST_MAIN(GdalTiledTest)
#include "gdal_tiled_t.moc"
