/***************************************************************************
 *   Copyright (C) 2012 by Daniel Nicoletti                                *
 *   dantti12@gmail.com                                                    *
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

#include "ColorD.h"

#include "Edid.h"

#include <KLocale>
#include <KGenericFactory>
#include <KNotification>
#include <KIcon>

#include <QX11Info>

#include <QFile>
#include <QCryptographicHash>
#include <QDBusMetaType>

K_PLUGIN_FACTORY(ColorDFactory, registerPlugin<ColorD>();)
K_EXPORT_PLUGIN(ColorDFactory("colord"))

bool has_1_2 = false;
bool has_1_3 = false;

ColorD::ColorD(QObject *parent, const QVariantList &args) :
    KDEDModule(parent)
{
    // There's not much use for args in a KCM
    Q_UNUSED(args)

    /* connect to colord using DBus */
    connectToColorD();

    /* Connect to the display */
    connectToDisplay();

    /* Scan all the *.icc files */
    scanHomeDirectory();

    qRegisterMetaType<StringStringMap>();
    qDBusRegisterMetaType<StringStringMap>();
}

ColorD::~ColorD()
{
}

/* This is what gnome-desktop does */
static quint8* getProperty(Display *dpy,
                           RROutput output,
                           Atom atom,
                           size_t &len)
{
    unsigned char *prop;
    int actual_format;
    unsigned long nitems, bytes_after;
    Atom actual_type;
    quint8 *result;

    XRRGetOutputProperty (dpy, output, atom,
                          0, 100, False, False,
                          AnyPropertyType,
                          &actual_type, &actual_format,
                          &nitems, &bytes_after, &prop);
    if (actual_type == XA_INTEGER && actual_format == 8) {
        result = new quint8[nitems];
        memcpy(result, prop, nitems);
        if (len) {
            len = nitems;
        }
    } else {
        result = NULL;
    }

    XFree (prop);
    return result;
}

quint8* ColorD::readEdidData(RROutput output, size_t &len)
{
    Atom edid_atom;
    quint8 *result;

    edid_atom = XInternAtom(m_dpy, RR_PROPERTY_RANDR_EDID, FALSE);
    result = getProperty(m_dpy, output, edid_atom, len);
    if (result == NULL) {
        edid_atom = XInternAtom(m_dpy, "EDID_DATA", FALSE);
        result = getProperty(m_dpy, output, edid_atom, len);
    }

    if (result) {
        if (len % 128 == 0) {
            return result;
        } else {
            delete result;
        }
    }

    return NULL;
}

void ColorD::addProfile(const QString &filename)
{
    const char *profile_id = "icc-unknown-hughsie";

    // open filename
    QFile profile(filename);
    if (!profile.open(QIODevice::ReadOnly)) {
        kWarning() << "Failed to open profile file:" << filename;
        return;
    }

    // get the MD5 hash of the contents
    QByteArray hash;
    hash = QCryptographicHash::hash(profile.readAll(), QCryptographicHash::Md5);

    // construct a profile-id from device-and-profile-naming-spec.txt
    //TODO
    //if (embedded_profile_id != NULL)
    //    profile_id = "icc-" + embedded_profile_id + username
    //else
    //    profile_id = "icc-" + hash + username

    //TODO: how to save these private to the class?
    QDBusMessage message;
    message = QDBusMessage::createMethodCall(QLatin1String("org.freedesktop.ColorManager"),
                                             QLatin1String("/org/freedesktop/ColorManager"),
                                             QLatin1String("org.freedesktop.ColorManager"),
                                             QLatin1String("CreateProfile"));
    StringStringMap properties;
    properties["Filename"] = filename;
    properties["FILE_checksum"] = hash;
    message << qVariantFromValue(QString(profile_id));
    message << qVariantFromValue(QString("temp"));
    message << qVariantFromValue(properties);

    QDBusReply<QDBusObjectPath> reply = QDBusConnection::systemBus().call(message, QDBus::BlockWithGui);

    kDebug() << "created profile" << reply.value().path();
}

void ColorD::scanHomeDirectory()
{
    /* Get a list of files in ~/.local/share/icc/ */
    //TODO

    /* Call AddProfile() for each file */
    //TODO
}

void ColorD::addOutput(RROutput output)
{
    QString edidVendor = QLatin1String("unknown");
    QString edidModel = QLatin1String("unknown");
    QString edidSerial = QLatin1String("unknown");
    QString deviceId = QLatin1String("xrandr-unknown");

    XRROutputInfo *info;
    info = XRRGetOutputInfo(m_dpy, m_resources, output);
    // ensure the RROutput is connected
    if (info == NULL || info->connection != RR_Connected) {
        return;
    }

    /* get the EDID */
    size_t size;
    const quint8 *data;
    data = readEdidData(output, size);
    if (data == NULL || size == 0) {
        kWarning() << "unable to get EDID for output";
        return;
    }

    // Created the Edid class which parses our info
    Edid *edid = new Edid(data, size);
    if (edid->isValid()) {
        kDebug() << "Edid Valid" << edid->deviceId(info->name);
        kDebug() << "Edid vendor" << edid->vendor();
        kDebug() << "Edid serial" << edid->serial();
        kDebug() << "Edid name" << edid->name();
        edidVendor = edid->vendor();
        edidModel = edid->name();
        edidSerial = edid->serial();
    }

    // grabing the device even if edid is not valid
    // if handles the fallback name if it's not valid
    deviceId = edid->deviceId(info->name);

    /* get the md5 of the EDID blob */
    //TODO

    /* parse the edid and save in a hash table [m_hash_edid_md5?]*/
    //TODO, and maybe c++ize http://git.gnome.org/browse/gnome-settings-daemon/tree/plugins/color/gcm-edid.c

    //TODO: how to save these private to the class?
    StringStringMap properties;
    properties["Kind"] = "display";
    properties["Mode"] = "physical";
    properties["Colorspace"] = "rgb";
    properties["Vendor"] = edidVendor;
    properties["Model"] = edidModel;
    properties["Serial"] = edidSerial;
    properties["XRANDR_name"] = info->name;

    /* call CreateDevice() with a device_id  */
    QDBusMessage message;
    message = QDBusMessage::createMethodCall(QLatin1String("org.freedesktop.ColorManager"),
                                             QLatin1String("/org/freedesktop/ColorManager"),
                                             QLatin1String("org.freedesktop.ColorManager"),
                                             QLatin1String("CreateDevice"));
    message << qVariantFromValue(deviceId);
    message << qVariantFromValue(QString("temp"));
    message << qVariantFromValue(properties);
    QDBusReply<QDBusObjectPath> reply = QDBusConnection::systemBus().call(message, QDBus::BlockWithGui);

    kDebug() << "created device" << reply.value().path();

    XRRFreeOutputInfo(info);
}


void ColorD::removeOutput(RROutput output)
{
    Q_UNUSED(output)
    /* find the device in colord using FindDeviceByProperty(info->name) */
    //TODO

    /* call DBus DeleteDevice() on the result */
    //TODO
}

void ColorD::connectToDisplay()
{
    m_dpy = QX11Info::display();

    // Check extension
    if (XRRQueryExtension(m_dpy, &m_eventBase, &m_errorBase) == false) {
        m_valid = false;
        return;
    }

    int major_version, minor_version;
    XRRQueryVersion(m_dpy, &major_version, &minor_version);

    m_version = i18n("X Resize and Rotate extension version %1.%2",
                     major_version,minor_version);

    // check if we have the new version of the XRandR extension
    has_1_2 = (major_version > 1 || (major_version == 1 && minor_version >= 2));
    has_1_3 = (major_version > 1 || (major_version == 1 && minor_version >= 3));

    if (has_1_3) {
        kDebug() << "Using XRANDR extension 1.3 or greater.";
    } else if (has_1_2) {
        kDebug() << "Using XRANDR extension 1.2.";
    } else {
        kDebug() << "Using legacy XRANDR extension (1.1 or earlier).";
    }

    kDebug() << "XRANDR error base: " << m_errorBase;

    /* XRRGetScreenResourcesCurrent is less expensive than
     * XRRGetScreenResources, however it is available only
     * in RandR 1.3 or higher
     */
    m_root = RootWindow(m_dpy, 0);

    if (has_1_3) {
        m_resources = XRRGetScreenResourcesCurrent(m_dpy, m_root);
    } else {
        m_resources = XRRGetScreenResources(m_dpy, m_root);
    }

    for (int i = 0; i < m_resources->noutput; ++i) {
        kDebug() << "Adding" << m_resources->outputs[i];
        addOutput(m_resources->outputs[i]);
    }

    /* register for root window changes */
    XRRSelectInput(m_dpy, m_root, RRScreenChangeNotifyMask);
//    gdk_x11_register_standard_event_type (m_dpy,
//                              m_eventBase,
//                              RRNotify + 1);
//    gdk_window_add_filter (priv->gdk_root, screen_on_event, self);

    /* if monitors are added, call AddOutput() */
    //TODO

    /* if monitors are removed, call RemoveOutput() */
    //TODO
}

void ColorD::profileAdded(const QDBusObjectPath &objectPath)
{
    /* check if the EDID_md5 Profile.Metadata matches any connected
     * XRandR devices (e.g. lvds1), otherwise ignore */
    //TODO

    /* call Device.AddProfile() with the device and profile object paths */
    //TODO
    kDebug() << "Profile added" << objectPath.path();
}

void ColorD::deviceAdded(const QDBusObjectPath &objectPath)
{
    kDebug() << "Device added" << objectPath.path();

    /* show a notification that the user should calibrate the device */
    //TODO
}

void ColorD::deviceChanged(const QDBusObjectPath &objectPath)
{
    kDebug() << "Device changed" << objectPath.path();

    /* check Device.Kind is "display" */
    //TODO

    /* read the default profile (the first path in the Device.Profiles
     * property) */
    //TODO

    /* open the physical file */
    //TODO

    /* read the VCGT data using lcms2 */
    //TODO

    /* push the data to the Xrandr gamma ramps for the display */
    //TODO

    /* export the file data as an x atom on the *screen* (not output) */
    //TODO: named _ICC_PROFILE
}

void ColorD::connectToColorD()
{
    // Creates a ColorD interface, it must be created with new
    // otherwise the object will be deleted when this block ends
    QDBusInterface *interface;
    interface = new QDBusInterface(QLatin1String("org.freedesktop.ColorManager"),
                                   QLatin1String("/org/freedesktop/ColorManager"),
                                   QLatin1String("org.freedesktop.ColorManager"),
                                   QDBusConnection::systemBus(),
                                   this);

    // listen to colord for events
    connect(interface, SIGNAL(ProfileAdded(QDBusObjectPath)),
            this, SLOT(profileAdded(QDBusObjectPath)));
    connect(interface, SIGNAL(DeviceAdded(QDBusObjectPath)),
            this, SLOT(deviceAdded(QDBusObjectPath)));
    connect(interface, SIGNAL(DeviceChanged(QDBusObjectPath)),
            this, SLOT(deviceChanged(QDBusObjectPath)));
}
