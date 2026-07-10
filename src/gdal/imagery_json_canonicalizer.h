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

#ifndef OPENORIENTEERING_IMAGERY_JSON_CANONICALIZER_H
#define OPENORIENTEERING_IMAGERY_JSON_CANONICALIZER_H

#include <QByteArray>
#include <QString>

class QJsonValue;

namespace OpenOrienteering {


/** RFC 8785 JSON Canonicalization Scheme encoder. */
class ImageryJsonCanonicalizer
{
public:
	static bool canonicalize(const QJsonValue& value, QByteArray* output, QString* error = nullptr);
	static QByteArray sha256(const QByteArray& bytes);
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_IMAGERY_JSON_CANONICALIZER_H
