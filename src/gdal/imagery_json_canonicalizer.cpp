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

#include "imagery_json_canonicalizer.h"

#include <algorithm>
#include <cmath>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

namespace OpenOrienteering {

namespace {

bool utf16Less(const QString& first, const QString& second)
{
	auto const common = std::min(first.size(), second.size());
	for (int i = 0; i < common; ++i)
	{
		auto const left = first.at(i).unicode();
		auto const right = second.at(i).unicode();
		if (left != right)
			return left < right;
	}
	return first.size() < second.size();
}


class Encoder
{
public:
	bool encode(const QJsonValue& value)
	{
		return encode(value, 0);
	}

	QByteArray result() const { return output; }
	QString errorString() const { return error; }

private:
	bool encode(const QJsonValue& value, int depth)
	{
		if (depth > 256)
			return fail(QStringLiteral("Canonical JSON nesting limit exceeded"));
		switch (value.type())
		{
		case QJsonValue::Null:
			output += "null";
			return true;
		case QJsonValue::Bool:
			output += value.toBool() ? "true" : "false";
			return true;
		case QJsonValue::Double:
			return encodeNumber(value.toDouble());
		case QJsonValue::String:
			return encodeString(value.toString());
		case QJsonValue::Array:
			return encodeArray(value.toArray(), depth + 1);
		case QJsonValue::Object:
			return encodeObject(value.toObject(), depth + 1);
		case QJsonValue::Undefined:
			return fail(QStringLiteral("Undefined is not a JSON value"));
		}
		return fail(QStringLiteral("Unknown JSON value type"));
	}

	bool encodeArray(const QJsonArray& array, int depth)
	{
		output += '[';
		for (int i = 0; i < array.size(); ++i)
		{
			if (i)
				output += ',';
			if (!encode(array.at(i), depth))
				return false;
		}
		output += ']';
		return true;
	}

	bool encodeObject(const QJsonObject& object, int depth)
	{
		auto keys = object.keys();
		std::sort(keys.begin(), keys.end(), utf16Less);
		output += '{';
		for (int i = 0; i < keys.size(); ++i)
		{
			if (i)
				output += ',';
			if (!encodeString(keys.at(i)))
				return false;
			output += ':';
			if (!encode(object.value(keys.at(i)), depth))
				return false;
		}
		output += '}';
		return true;
	}

	bool encodeString(const QString& string)
	{
		output += '"';
		for (int i = 0; i < string.size(); ++i)
		{
			auto const code_unit = string.at(i).unicode();
			switch (code_unit)
			{
			case 0x08: output += "\\b"; continue;
			case 0x09: output += "\\t"; continue;
			case 0x0a: output += "\\n"; continue;
			case 0x0c: output += "\\f"; continue;
			case 0x0d: output += "\\r"; continue;
			case '"': output += "\\\""; continue;
			case '\\': output += "\\\\"; continue;
			default: break;
			}
			if (code_unit < 0x20)
			{
				static const char hex[] = "0123456789abcdef";
				output += "\\u00";
				output += hex[(code_unit >> 4) & 0x0f];
				output += hex[code_unit & 0x0f];
				continue;
			}
			if (QChar::isHighSurrogate(code_unit))
			{
				if (i + 1 >= string.size() || !QChar::isLowSurrogate(string.at(i + 1).unicode()))
					return fail(QStringLiteral("String contains an unpaired high surrogate"));
				QString pair;
				pair.append(string.at(i));
				pair.append(string.at(++i));
				output += pair.toUtf8();
				continue;
			}
			if (QChar::isLowSurrogate(code_unit))
				return fail(QStringLiteral("String contains an unpaired low surrogate"));
			output += QString(string.at(i)).toUtf8();
		}
		output += '"';
		return true;
	}

	bool encodeNumber(double number)
	{
		if (!std::isfinite(number))
			return fail(QStringLiteral("NaN and infinity are not valid canonical JSON numbers"));
		if (number == 0)
		{
			output += '0';
			return true;
		}

		// Qt's JSON writer provides the shortest round-tripping decimal digits.
		// Reformat them below because Qt's exponent thresholds and spelling are
		// not ECMAScript/JCS compatible. RFC vectors pin this primitive against
		// future Qt changes.
		auto shortest = QJsonDocument(QJsonArray { number }).toJson(QJsonDocument::Compact);
		shortest = shortest.mid(1, shortest.size() - 2);
		bool negative = shortest.startsWith('-');
		if (negative)
			shortest.remove(0, 1);

		int exponent = 0;
		auto exponent_position = shortest.indexOf('e');
		if (exponent_position < 0)
			exponent_position = shortest.indexOf('E');
		QByteArray mantissa = shortest;
		if (exponent_position >= 0)
		{
			bool ok = false;
			exponent = shortest.mid(exponent_position + 1).toInt(&ok);
			if (!ok)
				return fail(QStringLiteral("Unable to parse shortest number exponent"));
			mantissa = shortest.left(exponent_position);
		}

		auto decimal_position = mantissa.indexOf('.');
		if (decimal_position < 0)
			decimal_position = mantissa.size();
		else
			mantissa.remove(decimal_position, 1);
		auto leading_zeroes = 0;
		while (leading_zeroes < mantissa.size() && mantissa.at(leading_zeroes) == '0')
			++leading_zeroes;
		mantissa.remove(0, leading_zeroes);
		decimal_position -= leading_zeroes;
		while (mantissa.size() > 1 && mantissa.endsWith('0'))
			mantissa.chop(1);
		if (mantissa.isEmpty())
			return fail(QStringLiteral("Unable to parse shortest number digits"));

		auto const decimal_point = decimal_position + exponent;
		if (negative)
			output += '-';
		if (decimal_point > 0 && decimal_point <= 21)
		{
			if (decimal_point >= mantissa.size())
			{
				output += mantissa;
				output += QByteArray(decimal_point - mantissa.size(), '0');
			}
			else
			{
				output += mantissa.left(decimal_point);
				output += '.';
				output += mantissa.mid(decimal_point);
			}
		}
		else if (decimal_point <= 0 && decimal_point > -6)
		{
			output += "0.";
			output += QByteArray(-decimal_point, '0');
			output += mantissa;
		}
		else
		{
			output += mantissa.at(0);
			if (mantissa.size() > 1)
			{
				output += '.';
				output += mantissa.mid(1);
			}
			output += 'e';
			auto const scientific_exponent = decimal_point - 1;
			if (scientific_exponent >= 0)
				output += '+';
			output += QByteArray::number(scientific_exponent);
		}
		return true;
	}

	bool fail(const QString& message)
	{
		error = message;
		return false;
	}

	QByteArray output;
	QString error;
};

}  // namespace


bool ImageryJsonCanonicalizer::canonicalize(const QJsonValue& value, QByteArray* output, QString* error)
{
	if (!output)
	{
		if (error)
			*error = QStringLiteral("Canonical JSON output pointer is null");
		return false;
	}
	Encoder encoder;
	if (!encoder.encode(value))
	{
		output->clear();
		if (error)
			*error = encoder.errorString();
		return false;
	}
	*output = encoder.result();
	if (error)
		error->clear();
	return true;
}


QByteArray ImageryJsonCanonicalizer::sha256(const QByteArray& bytes)
{
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}


}  // namespace OpenOrienteering
