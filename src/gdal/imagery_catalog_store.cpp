/*
 *    Copyright 2026 Ethan O'Connor
 *    This file is part of OpenOrienteering.
 *    SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "imagery_catalog_store.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include "imagery_json_canonicalizer.h"
#include "imagery_source_fingerprint.h"

namespace OpenOrienteering {

namespace {

bool save(const QString& path, const QByteArray& bytes, QString* error)
{
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
	{
		if (error)
			*error = file.errorString();
		return false;
	}
	return true;
}

QJsonObject stateObject(const ImageryCatalogReadResult& catalog,
	                    const QString& origin,
	                    const QByteArray& etag,
	                    const QByteArray& last_modified)
{
	QJsonArray sources;
	for (auto const& source : catalog.catalog.sources)
	{
		sources.push_back(QJsonObject {
			{ QStringLiteral("id"), source.id },
			{ QStringLiteral("fullFingerprint"), QString::fromLatin1(source.full_fingerprint) },
			{ QStringLiteral("operationalFingerprint"), QString::fromLatin1(source.operational_fingerprint) }
		});
	}
	return QJsonObject {
		{ QStringLiteral("origin"), origin },
		{ QStringLiteral("installedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
		{ QStringLiteral("sha256"), QString::fromLatin1(catalog.catalog.document_sha256) },
		{ QStringLiteral("etag"), QString::fromLatin1(etag) },
		{ QStringLiteral("lastModified"), QString::fromLatin1(last_modified) },
		{ QStringLiteral("fingerprintVersion"), ImagerySourceFingerprints::version },
		{ QStringLiteral("sources"), sources }
	};
}

}  // namespace


ImageryCatalogStore::ImageryCatalogStore(QString root)
{
	this->root = root.isEmpty()
	             ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("imagery-catalogs"))
	             : std::move(root);
}

QString ImageryCatalogStore::rootPath() const
{
	return root;
}

QString ImageryCatalogStore::directoryKey(const QString& catalog_id) const
{
	return QString::fromLatin1(ImageryJsonCanonicalizer::sha256(catalog_id.toUtf8()).left(32));
}

InstalledImageryCatalog ImageryCatalogStore::loadDirectory(const QString& directory, QString* error) const
{
	InstalledImageryCatalog installed;
	installed.directory = directory;
	QFile catalog_file(QDir(directory).filePath(QStringLiteral("catalog.") + ImageryCatalogReader::fileExtension()));
	if (!catalog_file.open(QIODevice::ReadOnly))
	{
		if (error) *error = catalog_file.errorString();
		return installed;
	}
	installed.read_result = ImageryCatalogReader::read(catalog_file.readAll());
	if (!installed.read_result.accepted())
	{
		if (error) *error = QStringLiteral("Installed catalog is invalid: %1").arg(directory);
		return installed;
	}
	QFile state_file(QDir(directory).filePath(QStringLiteral("state.json")));
	if (!state_file.open(QIODevice::ReadOnly))
	{
		if (error) *error = state_file.errorString();
		return installed;
	}
	auto const state = QJsonDocument::fromJson(state_file.readAll()).object();
	installed.state.origin = state.value(QStringLiteral("origin")).toString();
	installed.state.installed_at = QDateTime::fromString(state.value(QStringLiteral("installedAt")).toString(), Qt::ISODateWithMs);
	installed.state.sha256 = state.value(QStringLiteral("sha256")).toString().toLatin1();
	installed.state.etag = state.value(QStringLiteral("etag")).toString().toLatin1();
	installed.state.last_modified = state.value(QStringLiteral("lastModified")).toString().toLatin1();
	return installed;
}

QVector<InstalledImageryCatalog> ImageryCatalogStore::catalogs(QString* error) const
{
	QVector<InstalledImageryCatalog> installed;
	QDir directory(root);
	if (!directory.exists())
		return installed;
	for (auto const& name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
	{
		if (name.startsWith(QLatin1Char('.')))
			continue;
		QString load_error;
		auto catalog = loadDirectory(directory.filePath(name), &load_error);
		if (!catalog.read_result.accepted())
		{
			if (error) *error = load_error;
			continue;
		}
		installed.push_back(catalog);
	}
	return installed;
}

ImageryCatalogAnalysis ImageryCatalogStore::analyze(const ImageryCatalogReadResult& candidate, QString* error) const
{
	ImageryCatalogAnalysis analysis;
	for (auto const& issue : candidate.issues)
	{
		if (issue.type == ImageryCatalogIssue::Type::SourceError) ++analysis.invalid;
		else if (issue.type == ImageryCatalogIssue::Type::UnsupportedSource) ++analysis.unsupported;
	}
	auto const installed = catalogs(error);
	const InstalledImageryCatalog* previous = nullptr;
	for (auto const& catalog : installed)
		if (catalog.read_result.catalog.id == candidate.catalog.id)
			previous = &catalog;
	if (previous)
	{
		if (previous->read_result.catalog.revision < candidate.catalog.revision)
			analysis.update_kind = ImageryCatalogAnalysis::UpdateKind::HigherRevision;
		else if (previous->read_result.catalog.revision > candidate.catalog.revision)
			analysis.update_kind = ImageryCatalogAnalysis::UpdateKind::LowerRevision;
		else if (previous->state.sha256 == candidate.catalog.document_sha256)
			analysis.update_kind = ImageryCatalogAnalysis::UpdateKind::ExactReimport;
		else
			analysis.update_kind = ImageryCatalogAnalysis::UpdateKind::SameRevisionConflict;

		QMap<QString, QByteArray> old_sources;
		for (auto const& source : previous->read_result.catalog.sources)
			old_sources.insert(source.id, source.full_fingerprint);
		for (auto const& source : candidate.catalog.sources)
		{
			if (!old_sources.contains(source.id)) ++analysis.added;
			else if (old_sources.take(source.id) != source.full_fingerprint) ++analysis.changed;
			else old_sources.remove(source.id);
		}
		analysis.removed = old_sources.size();
	}
	else
	{
		analysis.added = candidate.catalog.sources.size();
	}

	for (auto const& source : candidate.catalog.sources)
	{
		for (auto const& catalog : installed)
		{
			if (catalog.read_result.catalog.id == candidate.catalog.id)
				continue;
			for (auto const& existing : catalog.read_result.catalog.sources)
			{
				if (source.full_fingerprint == existing.full_fingerprint) ++analysis.exact_duplicates;
				else if (source.operational_fingerprint == existing.operational_fingerprint) ++analysis.potential_duplicates;
			}
		}
	}
	return analysis;
}

bool ImageryCatalogStore::install(const ImageryCatalogReadResult& catalog,
	                              const QString& origin,
	                              const QByteArray& etag,
	                              const QByteArray& last_modified,
	                              QString* error) const
{
	if (!catalog.accepted())
	{
		if (error) *error = QStringLiteral("Catalog is not installable");
		return false;
	}
	if (!QDir().mkpath(root))
	{
		if (error) *error = QStringLiteral("Could not create catalog store");
		return false;
	}
	auto const key = directoryKey(catalog.catalog.id);
	auto const final_path = QDir(root).filePath(key);
	auto const staging_path = QDir(root).filePath(QLatin1Char('.') + key + QStringLiteral(".tmp-") + QUuid::createUuid().toString(QUuid::Id128));
	auto const backup_path = QDir(root).filePath(QLatin1Char('.') + key + QStringLiteral(".backup-") + QUuid::createUuid().toString(QUuid::Id128));
	if (!QDir().mkpath(staging_path)
	    || !save(QDir(staging_path).filePath(QStringLiteral("catalog.") + ImageryCatalogReader::fileExtension()), catalog.catalog.original_bytes, error)
	    || !save(QDir(staging_path).filePath(QStringLiteral("state.json")), QJsonDocument(stateObject(catalog, origin, etag, last_modified)).toJson(QJsonDocument::Indented), error))
	{
		QDir(staging_path).removeRecursively();
		return false;
	}
	auto had_previous = QDir(final_path).exists();
	if (had_previous && !QDir().rename(final_path, backup_path))
	{
		if (error) *error = QStringLiteral("Could not stage the previous catalog snapshot");
		QDir(staging_path).removeRecursively();
		return false;
	}
	if (!QDir().rename(staging_path, final_path))
	{
		if (had_previous) QDir().rename(backup_path, final_path);
		if (error) *error = QStringLiteral("Could not atomically install catalog snapshot");
		QDir(staging_path).removeRecursively();
		return false;
	}
	if (had_previous)
		QDir(backup_path).removeRecursively();
	return true;
}

bool ImageryCatalogStore::remove(const QString& catalog_id, QString* error) const
{
	auto const path = QDir(root).filePath(directoryKey(catalog_id));
	if (!QDir(path).exists())
		return true;
	if (!QDir(path).removeRecursively())
	{
		if (error) *error = QStringLiteral("Could not remove catalog snapshot");
		return false;
	}
	return true;
}

}  // namespace OpenOrienteering
