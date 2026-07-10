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

#ifndef OPENORIENTEERING_IMAGERY_CATALOG_H
#define OPENORIENTEERING_IMAGERY_CATALOG_H

#include <QByteArray>
#include <QDate>
#include <QJsonObject>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace OpenOrienteering {


struct TileMatrixDefinition
{
	QString id;
	double scale_denominator = 0;
	double cell_size = 0;
	QPointF point_of_origin;
	QString corner_of_origin = QStringLiteral("topLeft");
	QSize tile_size;
	qint64 matrix_width = 0;
	qint64 matrix_height = 0;
	bool has_variable_matrix_widths = false;
};


struct TileMatrixLimitsDefinition
{
	QString tile_matrix;
	qint64 min_tile_row = 0;
	qint64 max_tile_row = 0;
	qint64 min_tile_col = 0;
	qint64 max_tile_col = 0;
};


struct TileMatrixSetDefinition
{
	QString id;
	QString crs;
	QStringList ordered_axes;
	QVector<TileMatrixDefinition> tile_matrices;
	QJsonObject original_object;
	bool is_dyadic = false;
};


struct ImageryRequestDefinition
{
	QString referer;
	QVector<int> empty_http_status_codes;
};


struct ImageryRegistration
{
	enum class OperationType {
		None,
		Translation2d,
		Affine2d,
		GridShift,
	};

	OperationType operation_type = OperationType::None;
	QString direction;
	QString source_crs;
	QString target_crs;
	QString target_frame_id;
	QString unit;
	double dx = 0;
	double dy = 0;
	double xoff = 0;
	double yoff = 0;
	double s11 = 1;
	double s12 = 0;
	double s21 = 0;
	double s22 = 1;
	QString resource_id;
	QJsonObject original_object;
};


struct ImagerySourceDefinition
{
	QString id;
	QString name;
	QString type;
	QStringList tiles;
	QString scheme;
	QString format;
	QString min_tile_matrix;
	QString max_tile_matrix;
	QString tile_matrix_set_uri;
	TileMatrixSetDefinition tile_matrix_set;
	QVector<TileMatrixLimitsDefinition> tile_matrix_limits;
	ImageryRequestDefinition request;
	ImageryRegistration registration;
	QStringList required_capabilities;
	QStringList unsupported_capabilities;
	QByteArray full_fingerprint;
	QByteArray operational_fingerprint;
	QJsonObject original_object;
	bool valid = false;
	bool supported = false;
};


struct ImageryCatalog
{
	QString format;
	int version = 0;
	QString id;
	int revision = 0;
	QString name;
	QString description;
	QDate created;
	QDate updated;
	QStringList required_capabilities;
	QVector<ImagerySourceDefinition> sources;
	QJsonObject publisher;
	QJsonObject resources;
	QJsonObject extensions;
	QJsonObject original_object;
	QByteArray original_bytes;
	QByteArray document_sha256;
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_IMAGERY_CATALOG_H
