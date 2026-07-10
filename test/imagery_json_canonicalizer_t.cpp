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

#include <cstring>
#include <limits>

#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>

#include "gdal/imagery_json_canonicalizer.h"

namespace OpenOrienteering {


class ImageryJsonCanonicalizerTest : public QObject
{
	Q_OBJECT

private slots:
	void canonicalizationExample()
	{
		auto const document = QJsonDocument::fromJson(
			"{\"numbers\":[333333333.33333329,1E30,4.50,2e-3,0.000000000000000000000000001],"
			"\"string\":\"\\u20ac$\\u000f\\nA'B\\\"\\\\\\\\\\\"/\","
			"\"literals\":[null,true,false]}");
		QByteArray canonical;
		QString error;
		QVERIFY2(ImageryJsonCanonicalizer::canonicalize(document.object(), &canonical, &error), qPrintable(error));
		QCOMPARE(canonical, QString::fromUtf8(
			"{\"literals\":[null,true,false],\"numbers\":[333333333.3333333,1e+30,4.5,0.002,1e-27],"
			"\"string\":\"€$\\u000f\\nA'B\\\"\\\\\\\\\\\"/\"}").toUtf8());
	}

	void canonicalPropertyOrdering()
	{
		QJsonObject object {
			{ QString::fromUtf8("€"), QStringLiteral("Euro Sign") },
			{ QStringLiteral("\r"), QStringLiteral("Carriage Return") },
			{ QString::fromUtf8("דּ"), QStringLiteral("Hebrew Letter Dalet With Dagesh") },
			{ QStringLiteral("1"), QStringLiteral("One") },
			{ QString::fromUtf8("😀"), QStringLiteral("Emoji: Grinning Face") },
			{ QString::fromUtf8(""), QStringLiteral("Control") },
			{ QString::fromUtf8("ö"), QStringLiteral("Latin Small Letter O With Diaeresis") }
		};
		QByteArray canonical;
		QVERIFY(ImageryJsonCanonicalizer::canonicalize(object, &canonical));
		QCOMPARE(canonical, QString::fromUtf8(
			"{\"\\r\":\"Carriage Return\",\"1\":\"One\",\"\":\"Control\","
			"\"ö\":\"Latin Small Letter O With Diaeresis\",\"€\":\"Euro Sign\","
			"\"😀\":\"Emoji: Grinning Face\",\"דּ\":\"Hebrew Letter Dalet With Dagesh\"}").toUtf8());
	}

	void canonicalNumbers_data()
	{
		QTest::addColumn<qulonglong>("bits");
		QTest::addColumn<QByteArray>("expected");
		QTest::newRow("zero") << Q_UINT64_C(0x0000000000000000) << QByteArray("0");
		QTest::newRow("minus-zero") << Q_UINT64_C(0x8000000000000000) << QByteArray("0");
		QTest::newRow("min-positive") << Q_UINT64_C(0x0000000000000001) << QByteArray("5e-324");
		QTest::newRow("min-negative") << Q_UINT64_C(0x8000000000000001) << QByteArray("-5e-324");
		QTest::newRow("max-positive") << Q_UINT64_C(0x7fefffffffffffff) << QByteArray("1.7976931348623157e+308");
		QTest::newRow("max-negative") << Q_UINT64_C(0xffefffffffffffff) << QByteArray("-1.7976931348623157e+308");
		QTest::newRow("max-safe-plus-one") << Q_UINT64_C(0x4340000000000000) << QByteArray("9007199254740992");
		QTest::newRow("min-safe-minus-one") << Q_UINT64_C(0xc340000000000000) << QByteArray("-9007199254740992");
		QTest::newRow("two-to-68") << Q_UINT64_C(0x4430000000000000) << QByteArray("295147905179352830000");
		QTest::newRow("below-1e23") << Q_UINT64_C(0x44b52d02c7e14af5) << QByteArray("9.999999999999997e+22");
		QTest::newRow("1e23") << Q_UINT64_C(0x44b52d02c7e14af6) << QByteArray("1e+23");
		QTest::newRow("above-1e23") << Q_UINT64_C(0x44b52d02c7e14af7) << QByteArray("1.0000000000000001e+23");
		QTest::newRow("fixed-low") << Q_UINT64_C(0x444b1ae4d6e2ef4e) << QByteArray("999999999999999700000");
		QTest::newRow("fixed-high") << Q_UINT64_C(0x444b1ae4d6e2ef4f) << QByteArray("999999999999999900000");
		QTest::newRow("1e21") << Q_UINT64_C(0x444b1ae4d6e2ef50) << QByteArray("1e+21");
		QTest::newRow("below-1e-6") << Q_UINT64_C(0x3eb0c6f7a0b5ed8c) << QByteArray("9.999999999999997e-7");
		QTest::newRow("1e-6") << Q_UINT64_C(0x3eb0c6f7a0b5ed8d) << QByteArray("0.000001");
		QTest::newRow("third-low") << Q_UINT64_C(0x41b3de4355555553) << QByteArray("333333333.3333332");
		QTest::newRow("third-quarter") << Q_UINT64_C(0x41b3de4355555554) << QByteArray("333333333.33333325");
		QTest::newRow("third") << Q_UINT64_C(0x41b3de4355555555) << QByteArray("333333333.3333333");
		QTest::newRow("third-high") << Q_UINT64_C(0x41b3de4355555556) << QByteArray("333333333.3333334");
		QTest::newRow("third-higher") << Q_UINT64_C(0x41b3de4355555557) << QByteArray("333333333.33333343");
		QTest::newRow("negative-small-fixed") << Q_UINT64_C(0xbecbf647612f3696) << QByteArray("-0.0000033333333333333333");
		QTest::newRow("round-to-even") << Q_UINT64_C(0x43143ff3c1cb0959) << QByteArray("1424953923781206.2");
	}

	void canonicalNumbers()
	{
		QFETCH(qulonglong, bits);
		QFETCH(QByteArray, expected);
		double number = 0;
		std::memcpy(&number, &bits, sizeof(number));
		QByteArray canonical;
		QVERIFY(ImageryJsonCanonicalizer::canonicalize(number, &canonical));
		QCOMPARE(canonical, expected);
	}

	void canonicalizationErrorsAndHash()
	{
		QByteArray output;
		QString error;
		QVERIFY(!ImageryJsonCanonicalizer::canonicalize(std::numeric_limits<double>::infinity(), &output, &error));
		QVERIFY(!error.isEmpty());
		QVERIFY(!ImageryJsonCanonicalizer::canonicalize(QString(QChar(0xd800)), &output, &error));
		QCOMPARE(ImageryJsonCanonicalizer::sha256(QByteArray("abc")),
		         QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	}
};

}  // namespace OpenOrienteering

using OpenOrienteering::ImageryJsonCanonicalizerTest;

QTEST_APPLESS_MAIN(ImageryJsonCanonicalizerTest)
#include "imagery_json_canonicalizer_t.moc"
