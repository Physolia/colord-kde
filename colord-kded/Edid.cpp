/***************************************************************************
 *   Copyright (C) 2012-2016 by Daniel Nicoletti <dantti12@gmail.com>      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; see the file COPYING. If not, write to       *
 *   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,  *
 *   Boston, MA 02110-1301, USA.                                           *
 ***************************************************************************/

#include "Edid.h"

#include <math.h>

#include <QCryptographicHash>
#include <QFile>
#include <QStringList>

#include <QLoggingCategory>

#define GCM_EDID_OFFSET_PNPID 0x08
#define GCM_EDID_OFFSET_SERIAL 0x0c
#define GCM_EDID_OFFSET_SIZE 0x15
#define GCM_EDID_OFFSET_GAMMA 0x17
#define GCM_EDID_OFFSET_DATA_BLOCKS 0x36
#define GCM_EDID_OFFSET_LAST_BLOCK 0x6c
#define GCM_EDID_OFFSET_EXTENSION_BLOCK_COUNT 0x7e

#define GCM_DESCRIPTOR_DISPLAY_PRODUCT_NAME 0xfc
#define GCM_DESCRIPTOR_DISPLAY_PRODUCT_SERIAL_NUMBER 0xff
#define GCM_DESCRIPTOR_COLOR_MANAGEMENT_DATA 0xf9
#define GCM_DESCRIPTOR_ALPHANUMERIC_DATA_STRING 0xfe
#define GCM_DESCRIPTOR_COLOR_POINT 0xfb

#define PNP_IDS "/usr/share/hwdata/pnp.ids"

Q_DECLARE_LOGGING_CATEGORY(COLORD)

Edid::Edid()
{
}

Edid::Edid(const quint8 *data, size_t length)
{
    parse(data, length);
}

bool Edid::isValid() const
{
    return m_valid;
}

QString Edid::deviceId(const QString &fallbackName) const
{
    QString id = QStringLiteral("xrandr");
    // if no info was added check if the fallbacName is provided
    if (vendor().isNull() && name().isNull() && serial().isNull()) {
        if (!fallbackName.isEmpty()) {
            id.append(QLatin1Char('-') % fallbackName);
        } else {
            // all info we have are empty strings
            id.append(QLatin1String("-unknown"));
        }
    } else if (m_valid) {
        if (!vendor().isNull()) {
            id.append(QLatin1Char('-') % vendor());
        }
        if (!name().isNull()) {
            id.append(QLatin1Char('-') % name());
        }
        if (!serial().isNull()) {
            id.append(QLatin1Char('-') % serial());
        }
    }

    return id;
}

QString Edid::name() const
{
    if (m_valid) {
        return m_monitorName;
    }
    return QString();
}

QString Edid::vendor() const
{
    if (m_valid) {
        return m_vendorName;
    }
    return QString();
}

QString Edid::serial() const
{
    if (m_valid) {
        return m_serialNumber;
    }
    return QString();
}

QString Edid::eisaId() const
{
    if (m_valid) {
        return m_eisaId;
    }
    return QString();
}

QString Edid::hash() const
{
    if (m_valid) {
        return m_checksum;
    }
    return QString();
}

QString Edid::pnpId() const
{
    if (m_valid) {
        return m_pnpId;
    }
    return QString();
}

uint Edid::width() const
{
    return m_width;
}

uint Edid::height() const
{
    return m_height;
}

qreal Edid::gamma() const
{
    return m_gamma;
}

QQuaternion Edid::red() const
{
    return m_red;
}

QQuaternion Edid::green() const
{
    return m_green;
}

QQuaternion Edid::blue() const
{
    return m_blue;
}

QQuaternion Edid::white() const
{
    return m_white;
}

bool Edid::parse(const quint8 *data, size_t length)
{
    quint32 serial;

    /* check header */
    if (length < 128) {
        qCWarning(COLORD) << "EDID length is too small";
        m_valid = false;
        return m_valid;
    }
    if (data[0] != 0x00 || data[1] != 0xff) {
        qCWarning(COLORD) << "Failed to parse EDID header";
        m_valid = false;
        return m_valid;
    }

    /* decode the PNP ID from three 5 bit words packed into 2 bytes
     * /--08--\/--09--\
     * 7654321076543210
     * |\---/\---/\---/
     * R  C1   C2   C3 */
    m_pnpId[0] = 'A' + ((data[GCM_EDID_OFFSET_PNPID + 0] & 0x7c) / 4) - 1;
    m_pnpId[1] = 'A' + ((data[GCM_EDID_OFFSET_PNPID + 0] & 0x3) * 8) + ((data[GCM_EDID_OFFSET_PNPID + 1] & 0xe0) / 32) - 1;
    m_pnpId[2] = 'A' + (data[GCM_EDID_OFFSET_PNPID + 1] & 0x1f) - 1;

    // load the PNP_IDS file and load the vendor name
    if (!m_pnpId.isEmpty()) {
        QFile pnpIds(QLatin1String(PNP_IDS));
        if (pnpIds.open(QIODevice::ReadOnly)) {
            while (!pnpIds.atEnd()) {
                QString line = pnpIds.readLine();
                if (line.startsWith(m_pnpId)) {
                    QStringList parts = line.split(QLatin1Char('\t'));
                    if (parts.size() == 2) {
                        m_vendorName = line.split(QLatin1Char('\t')).at(1).simplified();
                    }
                    break;
                }
            }
            qCDebug(COLORD) << "PNP ID" << m_pnpId << "Vendor Name" << m_vendorName;
        }
    }

    /* maybe there isn't a ASCII serial number descriptor, so use this instead */
    serial = static_cast<quint32>(data[GCM_EDID_OFFSET_SERIAL + 0]);
    serial += static_cast<quint32>(data[GCM_EDID_OFFSET_SERIAL + 1] * 0x100);
    serial += static_cast<quint32>(data[GCM_EDID_OFFSET_SERIAL + 2] * 0x10000);
    serial += static_cast<quint32>(data[GCM_EDID_OFFSET_SERIAL + 3] * 0x1000000);
    if (serial > 0) {
        m_serialNumber = QString::number(serial);
    }

    /* get the size */
    m_width = data[GCM_EDID_OFFSET_SIZE + 0];
    m_height = data[GCM_EDID_OFFSET_SIZE + 1];

    /* we don't care about aspect */
    if (m_width == 0 || m_height == 0) {
        m_width = 0;
        m_height = 0;
    }

    /* get gamma */
    if (data[GCM_EDID_OFFSET_GAMMA] == 0xff) {
        m_gamma = 1.0f;
    } else {
        m_gamma = (static_cast<float>(data[GCM_EDID_OFFSET_GAMMA] / 100) + 1);
    }

    /* get color red */
    m_red.setX(edidDecodeFraction(data[0x1b], edidGetBits(data[0x19], 6, 7)));
    m_red.setY(edidDecodeFraction(data[0x1c], edidGetBits(data[0x19], 5, 4)));

    /* get color green */
    m_green.setX(edidDecodeFraction(data[0x1d], edidGetBits(data[0x19], 2, 3)));
    m_green.setY(edidDecodeFraction(data[0x1e], edidGetBits(data[0x19], 0, 1)));

    /* get color blue */
    m_blue.setX(edidDecodeFraction(data[0x1f], edidGetBits(data[0x1a], 6, 7)));
    m_blue.setY(edidDecodeFraction(data[0x20], edidGetBits(data[0x1a], 4, 5)));

    /* get color white */
    m_white.setX(edidDecodeFraction(data[0x21], edidGetBits(data[0x1a], 2, 3)));
    m_white.setY(edidDecodeFraction(data[0x22], edidGetBits(data[0x1a], 0, 1)));

    /* parse EDID data */
    for (uint i = GCM_EDID_OFFSET_DATA_BLOCKS; i <= GCM_EDID_OFFSET_LAST_BLOCK; i += 18) {
        /* ignore pixel clock data */
        if (data[i] != 0) {
            continue;
        }
        if (data[i + 2] != 0) {
            continue;
        }

        /* any useful blocks? */
        if (data[i + 3] == GCM_DESCRIPTOR_DISPLAY_PRODUCT_NAME) {
            QString tmp = edidParseString(&data[i + 5]);
            if (!tmp.isEmpty()) {
                m_monitorName = tmp;
            }
        } else if (data[i + 3] == GCM_DESCRIPTOR_DISPLAY_PRODUCT_SERIAL_NUMBER) {
            QString tmp = edidParseString(&data[i + 5]);
            if (!tmp.isEmpty()) {
                m_serialNumber = tmp;
            }
        } else if (data[i + 3] == GCM_DESCRIPTOR_COLOR_MANAGEMENT_DATA) {
            qCWarning(COLORD) << "failing to parse color management data";
        } else if (data[i + 3] == GCM_DESCRIPTOR_ALPHANUMERIC_DATA_STRING) {
            QString tmp = edidParseString(&data[i + 5]);
            if (!tmp.isEmpty()) {
                m_eisaId = tmp;
            }
        } else if (data[i + 3] == GCM_DESCRIPTOR_COLOR_POINT) {
            if (data[i + 3 + 9] != 0xff) {
                /* extended EDID block(1) which contains
                 * a better gamma value */
                m_gamma = ((float)data[i + 3 + 9] / 100) + 1;
            }
            if (data[i + 3 + 14] != 0xff) {
                /* extended EDID block(2) which contains
                 * a better gamma value */
                m_gamma = ((float)data[i + 3 + 9] / 100) + 1;
            }
        }
    }

    // calculate checksum
    QCryptographicHash hash(QCryptographicHash::Md5);
    hash.addData(reinterpret_cast<const char *>(data), length);
    m_checksum = hash.result().toHex();

    m_valid = true;
    return m_valid;
}

int Edid::edidGetBit(int in, int bit) const
{
    return (in & (1 << bit)) >> bit;
}

int Edid::edidGetBits(int in, int begin, int end) const
{
    int mask = (1 << (end - begin + 1)) - 1;

    return (in >> begin) & mask;
}

double Edid::edidDecodeFraction(int high, int low) const
{
    double result = 0.0;
    int i;

    high = (high << 2) | low;
    for (i = 0; i < 10; ++i) {
        result += edidGetBit(high, i) * pow(2, i - 10);
    }
    return result;
}

QString Edid::edidParseString(const quint8 *data) const
{
    QString text;

    /* this is always 13 bytes, but we can't guarantee it's null
     * terminated or not junk. */
    text = QString::fromLatin1((const char *)data, 13);

    // Remove newlines, extra spaces and stuff
    text = text.simplified();

    return text;
}
