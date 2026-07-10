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

#ifndef OPENORIENTEERING_IMAGERY_SOURCE_FINGERPRINT_H
#define OPENORIENTEERING_IMAGERY_SOURCE_FINGERPRINT_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include "imagery_catalog.h"

namespace OpenOrienteering {


struct ImagerySourceFingerprints
{
	static constexpr int version = 1;

	QByteArray full;
	QByteArray operational;
	QJsonObject normalized_full;
	QJsonObject normalized_operational;
};


class ImagerySourceFingerprint
{
public:
	static bool calculate(const ImagerySourceDefinition& source,
	                      ImagerySourceFingerprints* fingerprints,
	                      QString* error = nullptr);
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_IMAGERY_SOURCE_FINGERPRINT_H
