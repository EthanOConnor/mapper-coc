/*
 *    Copyright 2026 Ethan O'Connor
 *    This file is part of OpenOrienteering.
 *    SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENORIENTEERING_IMAGERY_CATALOG_STORE_H
#define OPENORIENTEERING_IMAGERY_CATALOG_STORE_H

#include <QDateTime>
#include <QString>
#include <QVector>

#include "imagery_catalog_reader.h"

namespace OpenOrienteering {

struct ImageryCatalogState
{
	QString origin;
	QDateTime installed_at;
	QByteArray sha256;
	QByteArray etag;
	QByteArray last_modified;
};

struct InstalledImageryCatalog
{
	ImageryCatalogReadResult read_result;
	ImageryCatalogState state;
	QString directory;
};

struct ImageryCatalogAnalysis
{
	enum class UpdateKind { NewCatalog, ExactReimport, HigherRevision, LowerRevision, SameRevisionConflict };
	UpdateKind update_kind = UpdateKind::NewCatalog;
	int added = 0;
	int changed = 0;
	int removed = 0;
	int invalid = 0;
	int unsupported = 0;
	int exact_duplicates = 0;
	int potential_duplicates = 0;
};

class ImageryCatalogStore
{
public:
	explicit ImageryCatalogStore(QString root = {});

	QString rootPath() const;
	QString directoryKey(const QString& catalog_id) const;
	QVector<InstalledImageryCatalog> catalogs(QString* error = nullptr) const;
	ImageryCatalogAnalysis analyze(const ImageryCatalogReadResult& candidate, QString* error = nullptr) const;
	bool install(const ImageryCatalogReadResult& catalog,
	             const QString& origin,
	             const QByteArray& etag = {},
	             const QByteArray& last_modified = {},
	             QString* error = nullptr) const;
	bool remove(const QString& catalog_id, QString* error = nullptr) const;

private:
	InstalledImageryCatalog loadDirectory(const QString& directory, QString* error) const;
	QString root;
};

}  // namespace OpenOrienteering

#endif
