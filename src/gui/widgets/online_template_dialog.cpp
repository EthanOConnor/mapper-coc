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

#include "online_template_dialog.h"

#include <utility>

#include <Qt>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include "core/georeferencing.h"
#include "core/map.h"
#include "gdal/imagery_catalog_reader.h"
#include "gdal/imagery_catalog_store.h"
#include "gdal/imagery_source_resolver.h"
#include "gdal/online_imagery_template_builder.h"
#include "gui/file_dialog.h"

namespace OpenOrienteering {

#ifndef MAPPER_USE_GDAL

OnlineTemplateDialog::OnlineTemplateDialog(
	Map& map,
	QString map_path,
	QRectF current_view_extent,
	QWidget* parent)
: QDialog(parent)
, map(map)
, map_path(std::move(map_path))
, current_view_extent(current_view_extent)
{
}

OnlineTemplateDialog::~OnlineTemplateDialog() = default;

void OnlineTemplateDialog::onAddClicked() {}
void OnlineTemplateDialog::onImportCatalogClicked() {}
void OnlineTemplateDialog::importCatalogFile(const QString&, const QString&) {}

#else

namespace {

constexpr int source_url_role = Qt::UserRole;
constexpr int source_name_role = Qt::UserRole + 1;
constexpr int catalog_id_role = Qt::UserRole + 2;
constexpr int catalog_source_id_role = Qt::UserRole + 3;
constexpr int max_recent_sources = 5;

}  // namespace


OnlineTemplateDialog::OnlineTemplateDialog(
	Map& map,
	QString map_path,
	QRectF current_view_extent,
	QWidget* parent)
: QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint)
, map(map)
, map_path(std::move(map_path))
, current_view_extent(std::move(current_view_extent))
{
	setWindowTitle(tr("Add online imagery template"));
	setMinimumWidth(470);

	auto* layout = new QVBoxLayout(this);

	auto* chooser_header = new QHBoxLayout();
	auto* chooser_label = new QLabel(tr("Imagery source:"), this);
	chooser_header->addWidget(chooser_label);
	chooser_header->addStretch();
	auto* import_button = new QPushButton(tr("Import catalog…"), this);
	import_button->setObjectName(QStringLiteral("import_catalog_button"));
	chooser_header->addWidget(import_button);
	layout->addLayout(chooser_header);

	source_chooser = new QComboBox(this);
	source_chooser->setObjectName(QStringLiteral("source_chooser"));
	populateSourceChooser();
	layout->addWidget(source_chooser);
	connect(source_chooser,
	        QOverload<int>::of(&QComboBox::activated),
	        this,
	        &OnlineTemplateDialog::onSourceChosen);

	auto* url_label = new QLabel(tr("Imagery Link (tile URL or ArcGIS MapServer):"), this);
	layout->addWidget(url_label);

	url_edit = new QLineEdit(this);
	url_edit->setObjectName(QStringLiteral("imagery_url"));
	url_edit->setPlaceholderText(tr("https://tile.example.com/{z}/{x}/{y}.png"));
	layout->addWidget(url_edit);
	connect(url_edit, &QLineEdit::textEdited, this, &OnlineTemplateDialog::onUrlEdited);

	auto* name_label = new QLabel(tr("Template Name:"), this);
	layout->addWidget(name_label);

	name_edit = new QLineEdit(this);
	name_edit->setFrame(true);
	name_edit->setMinimumHeight(url_edit->sizeHint().height());
	layout->addWidget(name_edit);

	auto* coverage_label = new QLabel(tr("Coverage:"), this);
	layout->addWidget(coverage_label);

	auto* coverage_layout = new QHBoxLayout();
	full_map_radio = new QRadioButton(tr("Full Map"), this);
	current_view_radio = new QRadioButton(tr("Current View"), this);
	current_view_radio->setObjectName(QStringLiteral("current_view_coverage"));
	full_map_radio->setChecked(true);
	coverage_layout->addWidget(full_map_radio);
	coverage_layout->addWidget(current_view_radio);
	coverage_layout->addStretch();
	layout->addLayout(coverage_layout);

	status_label = new QLabel(this);
	status_label->setWordWrap(true);
	status_label->hide();
	layout->addWidget(status_label);

	layout->addStretch();

	auto* button_layout = new QHBoxLayout();
	button_layout->addStretch();

	add_button = new QPushButton(tr("Add"), this);
	add_button->setDefault(true);
	button_layout->addWidget(add_button);

	auto* cancel_button = new QPushButton(tr("Cancel"), this);
	button_layout->addWidget(cancel_button);

	layout->addLayout(button_layout);

	connect(add_button, &QPushButton::clicked, this, &OnlineTemplateDialog::onAddClicked);
	connect(import_button, &QPushButton::clicked, this, &OnlineTemplateDialog::onImportCatalogClicked);
	connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
	connect(url_edit, &QLineEdit::returnPressed, this, &OnlineTemplateDialog::onAddClicked);
}


OnlineTemplateDialog::~OnlineTemplateDialog()
= default;


void OnlineTemplateDialog::onAddClicked()
{
	clearStatus();

	if (map_path.isEmpty())
	{
		setStatus(tr("Save the map first to add online imagery."), Qt::red, false);
		return;
	}

	if (map.getGeoreferencing().getState() != Georeferencing::Geospatial)
	{
		setStatus(tr("This map is not georeferenced yet. Online imagery needs a georeferenced map."), Qt::red, false);
		return;
	}

	QString coverage_error;
	selectedCoverageExtent(&coverage_error);
	if (!coverage_error.isEmpty())
	{
		setStatus(coverage_error, Qt::red, false);
		return;
	}

	if (has_selected_catalog_source)
	{
		pending_source = selected_catalog_source;
	}
	else
	{
		auto classify_result = OnlineImageryTemplateBuilder::classifyUrl(url_edit->text());
		if (!classify_result.error.isEmpty())
		{
			setStatus(classify_result.error, Qt::red, false);
			return;
		}
		pending_source = classify_result.source;
	}
	if (name_edit->text().trimmed().isEmpty())
		setSuggestedTemplateName(pending_source.display_name);

	generateAndAccept();
}


void OnlineTemplateDialog::setStatus(const QString& message, const QColor& color, bool italic)
{
	auto palette = status_label->palette();
	palette.setColor(QPalette::WindowText, color);
	status_label->setPalette(palette);
	auto font = status_label->font();
	font.setItalic(italic);
	status_label->setFont(font);
	status_label->setText(message);
	status_label->show();
}


void OnlineTemplateDialog::clearStatus()
{
	status_label->hide();
	status_label->clear();
}


void OnlineTemplateDialog::generateAndAccept()
{
	setStatus(tr("Preparing imagery template..."), Qt::gray, true);

	QString coverage_error;
	auto extent = selectedCoverageExtent(&coverage_error);
	if (!coverage_error.isEmpty())
	{
		setStatus(coverage_error, Qt::red, false);
		return;
	}

	auto result = OnlineImageryTemplateBuilder::generateXml(
		pending_source,
		effectiveTemplateName(pending_source),
		extent,
		map.getGeoreferencing(),
		map_path);
	if (!result.error.isEmpty())
	{
		setStatus(result.error, Qt::red, false);
		return;
	}

	if (result.area_km2 > OnlineImageryTemplateBuilder::area_warning_threshold_km2)
	{
		auto answer = QMessageBox::warning(
			this,
			tr("Large coverage area"),
			tr("This online template would cover about %1 km². Continue?")
				.arg(QString::number(result.area_km2, 'f', 0)),
			QMessageBox::Yes | QMessageBox::No,
			QMessageBox::No);
		if (answer != QMessageBox::Yes)
		{
			clearStatus();
			return;
		}
	}

	generated_path = result.xml_path;
	if (!has_selected_catalog_source)
		saveRecentSource(url_edit->text().trimmed(), pending_source.display_name);
	accept();
}


void OnlineTemplateDialog::onImportCatalogClicked()
{
	auto const filter = tr("Imagery catalogs (*.%1)").arg(ImageryCatalogReader::fileExtension());
	auto const path = FileDialog::getOpenFileName(this, tr("Import imagery catalog"), {}, filter);
	if (!path.isEmpty())
		importCatalogFile(path);
}


void OnlineTemplateDialog::importCatalogFile(const QString& file_path, const QString& store_root)
{
	clearStatus();
	if (!store_root.isEmpty())
		catalog_store_root = store_root;
	QFile file(file_path);
	if (!file.open(QIODevice::ReadOnly))
	{
		setStatus(tr("Could not open catalog: %1").arg(file.errorString()), Qt::red, false);
		return;
	}

	auto const candidate = ImageryCatalogReader::read(file.readAll());
	if (!candidate.accepted())
	{
		auto const detail = candidate.issues.isEmpty()
		                    ? tr("The file is not a valid imagery catalog.")
		                    : candidate.issues.first().message;
		setStatus(tr("Could not import catalog: %1").arg(detail), Qt::red, false);
		return;
	}

	QString error;
	ImageryCatalogStore store(catalog_store_root);
	if (!store.install(candidate, QUrl::fromLocalFile(file_path).toString(), {}, {}, &error))
	{
		setStatus(tr("Could not save catalog: %1").arg(error), Qt::red, false);
		return;
	}

	populateSourceChooser();
	int first_source = -1;
	for (int i = 0; i < source_chooser->count(); ++i)
	{
		if (source_chooser->itemData(i, catalog_id_role).toString() == candidate.catalog.id)
		{
			first_source = i;
			break;
		}
	}
	if (first_source >= 0)
	{
		source_chooser->setCurrentIndex(first_source);
		onSourceChosen(first_source);
	}
	setStatus(tr("Imported %1.").arg(candidate.catalog.name), Qt::darkGreen, false);
}


void OnlineTemplateDialog::populateSourceChooser()
{
	has_selected_catalog_source = false;
	source_chooser->clear();
	source_chooser->addItem(tr("Choose an imagery source"));

	ImageryCatalogStore store(catalog_store_root);
	auto const catalogs = store.catalogs();
	bool added_catalog_source = false;
	for (auto const& installed : catalogs)
	{
		for (auto const& source : installed.read_result.catalog.sources)
		{
			if (!source.supported)
				continue;
			if (!added_catalog_source)
			{
				source_chooser->insertSeparator(source_chooser->count());
				added_catalog_source = true;
			}
			auto const index = source_chooser->count();
			source_chooser->addItem(source.name);
			source_chooser->setItemData(index, installed.read_result.catalog.id, catalog_id_role);
			source_chooser->setItemData(index, source.id, catalog_source_id_role);
			source_chooser->setItemData(index, source.name, source_name_role);
		}
	}

	QSettings settings;
	auto recent_urls = settings.value(QStringLiteral("onlineImagery/recentUrls")).toStringList();
	auto recent_names = settings.value(QStringLiteral("onlineImagery/recentNames")).toStringList();
	if (!recent_urls.isEmpty())
	{
		source_chooser->insertSeparator(source_chooser->count());
		for (int i = 0; i < recent_urls.size(); ++i)
		{
			auto const name = (i < recent_names.size() && !recent_names[i].isEmpty())
			                  ? recent_names[i]
			                  : recent_urls[i];
			auto index = source_chooser->count();
			source_chooser->addItem(name);
			source_chooser->setItemData(index, recent_urls[i], source_url_role);
			source_chooser->setItemData(index, name, source_name_role);
		}
	}

	source_chooser->setCurrentIndex(0);
}


void OnlineTemplateDialog::onUrlEdited(const QString& text)
{
	Q_UNUSED(text)
	has_selected_catalog_source = false;
	source_name_hint.clear();
	if (source_chooser->currentIndex() != 0)
		source_chooser->setCurrentIndex(0);
	setSuggestedTemplateName(inferredSourceNameForUrl());
}


void OnlineTemplateDialog::onSourceChosen(int index)
{
	auto const catalog_id = source_chooser->itemData(index, catalog_id_role).toString();
	auto const catalog_source_id = source_chooser->itemData(index, catalog_source_id_role).toString();
	if (!catalog_id.isEmpty() && !catalog_source_id.isEmpty())
	{
		for (auto const& installed : ImageryCatalogStore(catalog_store_root).catalogs())
		{
			if (installed.read_result.catalog.id != catalog_id)
				continue;
			for (auto const& definition : installed.read_result.catalog.sources)
			{
				if (definition.id != catalog_source_id)
					continue;
				auto const resolved = ImagerySourceResolver::resolve(definition);
				if (!resolved.error.isEmpty())
				{
					setStatus(resolved.error, Qt::red, false);
					return;
				}
				selected_catalog_source = resolved.source;
				has_selected_catalog_source = true;
				source_name_hint = resolved.source.display_name;
				url_edit->setText(resolved.source.normalized_url);
				setSuggestedTemplateName(source_name_hint);
				return;
			}
		}
	}

	has_selected_catalog_source = false;
	auto url = source_chooser->itemData(index, source_url_role).toString();
	source_name_hint = source_chooser->itemData(index, source_name_role).toString();
	if (!url.isEmpty())
	{
		url_edit->setText(url);
		setSuggestedTemplateName(source_name_hint);
	}
}


void OnlineTemplateDialog::setSuggestedTemplateName(const QString& source_name)
{
	name_edit->setText(suggestedTemplateName(source_name));
}


QString OnlineTemplateDialog::mapDisplayName() const
{
	QFileInfo map_info(map_path);
	auto name = map_info.completeBaseName().trimmed();
	name.replace(QLatin1Char('_'), QLatin1Char(' '));
	name.replace(QLatin1Char('-'), QLatin1Char(' '));
	return name;
}


QString OnlineTemplateDialog::inferredSourceNameForUrl() const
{
	if (!source_name_hint.isEmpty())
		return source_name_hint;

	auto classify_result = OnlineImageryTemplateBuilder::classifyUrl(url_edit->text());
	return classify_result.error.isEmpty() ? classify_result.source.display_name : QString{};
}


QString OnlineTemplateDialog::suggestedTemplateName(const QString& source_name) const
{
	auto const trimmed_source_name = source_name.trimmed();
	auto const map_name = mapDisplayName();
	if (trimmed_source_name.isEmpty())
		return {};
	if (map_name.isEmpty())
		return trimmed_source_name;
	return tr("%1 - %2").arg(trimmed_source_name, map_name);
}


QString OnlineTemplateDialog::effectiveTemplateName(const OnlineImagerySource& source) const
{
	auto const trimmed_name = name_edit->text().trimmed();
	return trimmed_name.isEmpty() ? suggestedTemplateName(source.display_name) : trimmed_name;
}


QRectF OnlineTemplateDialog::selectedCoverageExtent(QString* error) const
{
	auto const coverage_mode = current_view_radio->isChecked()
	                           ? CoverageMode::CurrentView
	                           : CoverageMode::FullMap;
	switch (coverage_mode)
	{
	case CoverageMode::CurrentView:
		if (current_view_extent.isEmpty())
		{
			if (error)
				*error = tr("Could not determine the current view area.");
			return {};
		}
		return current_view_extent;

	case CoverageMode::FullMap:
	{
		auto extent = map.calculateExtent(false, false, nullptr);
		if (extent.isEmpty() && error)
			*error = tr("The map has no objects yet. Use Current View instead.");
		return extent;
	}
	}

	Q_UNREACHABLE();
}


void OnlineTemplateDialog::saveRecentSource(const QString& url, const QString& display_name)
{
	QSettings settings;
	auto urls = settings.value(QStringLiteral("onlineImagery/recentUrls")).toStringList();
	auto names = settings.value(QStringLiteral("onlineImagery/recentNames")).toStringList();

	auto existing = urls.indexOf(url);
	if (existing >= 0)
	{
		urls.removeAt(existing);
		if (existing < names.size())
			names.removeAt(existing);
	}

	urls.prepend(url);
	names.prepend(display_name);

	while (urls.size() > max_recent_sources)
		urls.removeLast();
	while (names.size() > max_recent_sources)
		names.removeLast();

	settings.setValue(QStringLiteral("onlineImagery/recentUrls"), urls);
	settings.setValue(QStringLiteral("onlineImagery/recentNames"), names);
}

#endif


}  // namespace OpenOrienteering
