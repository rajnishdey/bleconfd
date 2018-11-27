#include "bluetoothapplication.h"

#include <QtBluetooth/QBluetoothHostInfo>
#include <QtBluetooth/QBluetoothLocalDevice>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTimer>

#include <iostream>

static QBluetoothUuid const kRdkProvisionService = QBluetoothUuid(QString("8df5ad72-9bbc-4167-bcd9-e8eb9e4d671b"));
static QBluetoothUuid const kRdkWiFiConfigCharacteristic = QBluetoothUuid(QString("b87a896b-4052-4cab-a7e7-a71594d9c353"));
static QBluetoothUuid const kRdkPublicKeyCharacteristic = QBluetoothUuid(QString("cb9fee4d-c6ed-48c1-ab46-c3f2da38eedd"));
static QBluetoothUuid const kRdkProvisionStateCharacteristic = QBluetoothUuid(QString("79defbc1-eb45-448d-9f2a-1ecc3a47a242"));


static QBluetoothUuid const kRdkInboxCharacteristic = QBluetoothUuid(QString("cb163929-9a3d-4a1e-826f-e7e7cb2039e8"));

static QString propertiesToString(QLowEnergyCharacteristic::PropertyTypes props)
{
  QStringList list;
  if (props & QLowEnergyCharacteristic::Unknown)
    list.append("Unknown");
  if (props & QLowEnergyCharacteristic::Broadcasting)
    list.append("Broadcasting");
  if (props & QLowEnergyCharacteristic::Read)
    list.append("Read");
  if (props & QLowEnergyCharacteristic::WriteNoResponse)
    list.append("WriteNoResponse");
  if (props & QLowEnergyCharacteristic::Write)
    list.append("Write");
  if (props & QLowEnergyCharacteristic::Notify)
    list.append("Notify");
  if (props & QLowEnergyCharacteristic::Indicate)
    list.append("Indicate");
  if (props & QLowEnergyCharacteristic::WriteSigned)
    list.append("WriteSigned");
  if (props & QLowEnergyCharacteristic::ExtendedProperty)
    list.append("ExtendedProperty");
  return "[" + list.join(',') + "]";
}

static QMap<QBluetoothUuid, QString> kRDKCharacteristicUUIDs = 
{
  { kRdkProvisionStateCharacteristic, "Provision State"},
  { kRdkPublicKeyCharacteristic, "Public Key"},
  { kRdkWiFiConfigCharacteristic, "WiFi Config"},

  { QBluetoothUuid(QString("12984c43-3b43-4952-a387-715dcf9795c6")), "DeviceId"},
  { QBluetoothUuid(QString("16abb396-ab2c-4928-973d-e28a406d042b")), "Lost And Found Access Token"}
};

static uint16_t
ST(uint16_t n)
{
  return n;
}

static QMap< uint16_t, QString > kRdkStatusStrings = 
{
  { ST(0x0101), "Awaiting WiFi Configuration" },
  { ST(0x0102), "Processing WiFi Configuration" },
  { ST(0x0103), "Connecting to WiFi" },
  { ST(0x0104), "Successfully conected to WiFi" },
  { ST(0x0105), "Acquiring IP Address" },
  { ST(0x0106), "IP address acquired" },
  { ST(0x09ff), "Setup complete" },
  
  { ST(0x0a01), "Unknown Error" },
  { ST(0x0a02), "Error decrytingA WiFi configuration" },
  { ST(0x0a03), "Malformed WiFi configuration" },
  { ST(0x0a04), "Acquire IP timeout" }
};

QString
statusToString(QByteArray const& arr)
{
  uint16_t n = arr[1];
  n <<= 8;
  n |= arr[0];

  QString s;
  QMap< uint16_t, QString >::const_iterator i = kRdkStatusStrings.find(n);
  if (i != kRdkStatusStrings.end())
  {
    s = QString("(%1) %2").arg(QString::asprintf("0x%04x",n)).arg(i.value());
  }
  else
    s = QString::number(n);

  return s;
}

static QString charName(QLowEnergyCharacteristic const& c)
{
  QString name = c.name();
  if (name.isEmpty() && kRDKCharacteristicUUIDs.contains(c.uuid()))
    name = kRDKCharacteristicUUIDs[c.uuid()];
  return name;
}

static QString
byteArrayToString(QBluetoothUuid const& uuid, QByteArray const& a)
{
  QString s;
  if (uuid == kRdkProvisionStateCharacteristic)
    return statusToString(a);

  bool allAscii = true;
  for (char c : a)
  {
    if (!QChar::isPrint(c))
    {
      allAscii = false;
      break;
    }
  }

  if (allAscii)
    s = QString::fromLatin1(a);
  else
    s = a.toHex(',');

  return s;
}


BluetoothApplication::BluetoothApplication(QBluetoothAddress const& adapter)
  : m_disco_agent(nullptr)
  , m_adapter(adapter)
  , m_controller(nullptr)
  , m_rdk_service(nullptr)
  , m_poll_status(true)
  , m_network_access_manager(nullptr)
  , m_do_full_provision(false)
{
  log("local adapter is null:%d", m_adapter.isNull());
  foreach (QBluetoothHostInfo info, QBluetoothLocalDevice::allDevices())
  {
    QBluetoothLocalDevice adapter(info.address());
    log("local adapter:%s/%s", qPrintable(info.name()), qPrintable(info.address().toString()));
  }
}

void
BluetoothApplication::run(QBluetoothAddress const& addr)
{
  configureDevice(addr);
}
  

void
BluetoothApplication::configureDevice(QBluetoothAddress const& addr)
{
  if (!m_adapter.isNull())
    m_disco_agent = new QBluetoothDeviceDiscoveryAgent(m_adapter);
  else
    m_disco_agent = new QBluetoothDeviceDiscoveryAgent();

  connect(m_disco_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
    [=](QBluetoothDeviceInfo const& info)
    {
      if (info.address() == addr)
      {
        log("found match with addr:%s", qPrintable(info.address().toString()));
        m_disco_agent->stop();
        connectToDevice(info);
      }
    });

  startOp("discovery");
  m_disco_agent->start();
}

void
BluetoothApplication::connectToDevice(QBluetoothDeviceInfo const& info)
{
  log("connecting to:%s", qPrintable(info.address().toString())); 

  m_device_info = info;
  m_controller = QLowEnergyController::createCentral(m_device_info);
  connect(m_controller, &QLowEnergyController::connected, this,
    &BluetoothApplication::onDeviceConnected);
  startOp("connect");
  m_controller->connectToDevice();
}

void
BluetoothApplication::onDeviceConnected()
{
  endOp("connect");
  connect(m_controller, &QLowEnergyController::discoveryFinished, this,
    &BluetoothApplication::onDiscoveryFinished);
  connect(m_controller, QOverload<QLowEnergyController::Error>::of(&QLowEnergyController::error),
    [this](QLowEnergyController::Error err) { log("controller error:%d", err); });

  startOp("discovery");
  m_controller->discoverServices();
}

void
BluetoothApplication::onDiscoveryFinished()
{
  endOp("discovery");
  for (QBluetoothUuid const& uuid : m_controller->services())
  {
    QLowEnergyService* service = m_controller->createServiceObject(uuid);

    log("found service uuid:%s name:%s", qPrintable(service->serviceUuid().toString()),
      qPrintable(service->serviceName()));


    if (service->serviceUuid() == kRdkProvisionService)
    {
      log("found RDK setup service");
      m_rdk_service = service;
      connect(m_rdk_service, &QLowEnergyService::characteristicChanged,
          this, &BluetoothApplication::onCharacteristicChanged);
    }

    m_discovered_services.append(service);
  }

  m_service_iterator = m_discovered_services.begin();
  introspectNextService();
}

void
BluetoothApplication::introspectNextService()
{
  if (m_service_iterator != m_discovered_services.end())
  {
    QLowEnergyService* s = *m_service_iterator;

    QString msg = QString::asprintf("introspec %s/%s", qPrintable(s->serviceUuid().toString()),
      qPrintable(s->serviceName()));
    startOp(qPrintable(msg));
    connect(s, &QLowEnergyService::stateChanged, this, &BluetoothApplication::onServiceStateChanged);
    connect(s, QOverload<QLowEnergyService::ServiceError>::of(&QLowEnergyService::error),
      [this](QLowEnergyService::ServiceError err)
      {
        log("QLowEnergyService Error:%d", err);
      });

    s->discoverDetails();
  }
  else
  {
    log("done discovering services and characteristics");

    // clear all connected slots
    for (auto svc : m_discovered_services)
      svc->disconnect();


    log("TODO: remove this statement");
    quit();

    // connect for updates to status
    connect(m_rdk_service, &QLowEnergyService::characteristicChanged,
      this, &BluetoothApplication::onCharacteristicChanged);

    // until we get the notify working
    if (m_poll_status)
      pollStatus();

    // XXX
    QLowEnergyCharacteristic pubKey = m_rdk_service->characteristic(kRdkPublicKeyCharacteristic);
    log("uuid pubkey:%s", qPrintable(pubKey.uuid().toString()));
    // getWiFiConfiguration(m_xbo_account_id, m_client_secret);
  }
}

void
BluetoothApplication::readNextCharacteristic()
{
  if (m_chars_iterator == m_discovered_chars.end())
  {
    m_discovered_chars.clear();
    m_service_iterator++;
    introspectNextService();
    return;
  }

  QLowEnergyService* s = *m_service_iterator;
  QLowEnergyCharacteristic c = *m_chars_iterator;

  if (c.properties() & QLowEnergyCharacteristic::Read)
  {
    m_chars_iterator++;
    s->readCharacteristic(c);
  }
  else
  {
    log("property is not readable");
    QString name = charName(c);
    log("\tcharacteristic uuid:%s name:%s", qPrintable(c.uuid().toString()), qPrintable(name));
    log("\t\tprops:%s", qPrintable(propertiesToString(c.properties())));
    log("\t\tvalue: [not readable]");
    m_chars_iterator++;
    QTimer::singleShot(100, [this] { readNextCharacteristic(); });
  }
}

void
BluetoothApplication::onServiceStateChanged(QLowEnergyService::ServiceState newState)
{
  if (newState != QLowEnergyService::ServiceDiscovered)
  {
    log("unhandled service state:%d", newState);
    return;
  }

  QLowEnergyService* s = *m_service_iterator;

  QString msg = QString::asprintf("introspec %s/%s", qPrintable(s->serviceUuid().toString()),
    qPrintable(s->serviceName()));
  endOp(qPrintable(msg));

  connect(s, &QLowEnergyService::characteristicRead, [=](QLowEnergyCharacteristic const& c, QByteArray const& value) {
    QString name = charName(c);
    log("\tcharacteristic uuid:%s name:%s", qPrintable(c.uuid().toString()), qPrintable(name));
    log("\t\tprops:%s", qPrintable(propertiesToString(c.properties())));
    log("\t\tvalue:%s", qPrintable(byteArrayToString(c.uuid(), value)));

    // check to see if rdkc status char has notify enabled. if it does, then we can
    // register for notification instead of polling

    // This code actually enables notification remotely. This char is actually
    // read-only, so we don't need to do this. The XW4 already has notify set.
    // A callback for characteristicChanged is already connected

    #if 0
    if (c.uuid() == kRdkProvisionStateCharacteristic)
    {
      auto props = c.properties();
      if (props & QLowEnergyCharacteristic::Notify)
      {
        auto desc = c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration);
        if (desc.isValid())
        {
          s->writeDescriptor(desc, QByteArray::fromHex("0100"));
          log("enabled notification on %s", qPrintable(charName(c)));
        }
      }
    }
    #endif

    // If the status char has notify, no need to poll
    if ((c.uuid() == kRdkProvisionStateCharacteristic) && (c.properties() & QLowEnergyCharacteristic::Notify))
    {
      log("setting poll status to false, since %s has notify enabled",
        qPrintable(c.uuid().toString()));
      m_poll_status = false;
    }

    if (c.uuid() == QBluetoothUuid(QString("00002a23-0000-1000-8000-00805f9b34fb")))
      m_json_info["systemId"] = qPrintable(byteArrayToString(c.uuid(), value));
    else if (c.uuid() == QBluetoothUuid(QString("00002a25-0000-1000-8000-00805f9b34fb")))
      m_json_info["serialNumber"]= qPrintable(byteArrayToString(c.uuid(), value));
    else if (c.uuid() == QBluetoothUuid(QString("00002a24-0000-1000-8000-00805f9b34fb")))
      m_json_info["model"] = qPrintable(byteArrayToString(c.uuid(), value));
    else if (c.uuid() == kRdkPublicKeyCharacteristic)
      m_public_key = byteArrayToString(c.uuid(), value);

    if (m_use_pubkey)
      m_json_info["publicKey"] = m_public_key;

    readNextCharacteristic();
  });

  m_discovered_chars.clear();
  for (QLowEnergyCharacteristic const& c : s->characteristics())
  {
    if (c.uuid() == kRdkProvisionStateCharacteristic)
      m_rdk_status_char = c;
    if (c.uuid() == kRdkWiFiConfigCharacteristic)
      m_rdk_wifi_char = c;
    m_discovered_chars.append(c);
  }
  m_chars_iterator = m_discovered_chars.begin();

  readNextCharacteristic();
}

void
BluetoothApplication::log(char const* format, ...)
{
  QDateTime now = QDateTime::currentDateTime();
  printf("%s : ", qPrintable(now.toString(Qt::ISODate)));

  va_list args;
  va_start(args, format);
  vprintf(format, args);
  printf("\n");
  va_end(args);
}

void
BluetoothApplication::pollStatus()
{
  log("starting polling timer for status");
  connect(m_rdk_service, &QLowEnergyService::characteristicRead, [=](QLowEnergyCharacteristic const& c, QByteArray const& value) {
    QTimer::singleShot(1000, [this, c, value]
    {
      QString name = charName(c);
      log("characterisitic READ uuid:%s name:%s", qPrintable(c.uuid().toString()),
        qPrintable(name));
      log("\tprops:%s", qPrintable(propertiesToString(c.properties())));
      log("\tvalue:%s", qPrintable(byteArrayToString(c.uuid(), value)));
      
      m_rdk_service->readCharacteristic(m_rdk_status_char);
    });
  });

  m_rdk_service->readCharacteristic(m_rdk_status_char);

}

void
BluetoothApplication::onCharacteristicChanged(QLowEnergyCharacteristic const& c, QByteArray const& value)
{
  QString name = c.name();
  if (name.isEmpty() && kRDKCharacteristicUUIDs.contains(c.uuid()))
    name = kRDKCharacteristicUUIDs[c.uuid()];
  log("characterisitic CHANGED uuid:%s name:%s", qPrintable(c.uuid().toString()),
    qPrintable(name));
  log("\tprops:%s", qPrintable(propertiesToString(c.properties())));
  log("\tvalue:%s", qPrintable(byteArrayToString(c.uuid(), value)));
}

void
BluetoothApplication::startOp(char const* op)
{
  log("[%s] -- starting", op);

  QString key = QString::fromUtf8(op);
  if (m_times.contains(key))
    return;

  TimeSpan& span = m_times[key];
  span.StartTime = QDateTime::currentDateTimeUtc();
  span.Ended = false;
}

void
BluetoothApplication::endOp(char const* op)
{
  log("[%s] -- complete", op);

  QString key = QString::fromUtf8(op);
  TimeSpan& span = m_times[key];
  span.EndTime = QDateTime::currentDateTimeUtc();
  span.Ended = true;
}

void
BluetoothApplication::quit()
{
  if (m_controller)
  {
    log("disconnecting from device");
    m_controller->disconnectFromDevice();
  }
  printTimes();
  QCoreApplication::quit();
}

void
BluetoothApplication::printTimes()
{
  std::cout << "--------------- BEGIN TIMINGS ---------------" << std::endl;
  for (auto key : m_times.keys())
  {
    TimeSpan const& span = m_times.value(key);
    if (span.Ended)
    {
      qint64 end = span.EndTime.toMSecsSinceEpoch();
      qint64 start = span.StartTime.toMSecsSinceEpoch();
      printf("%s:%lldms\n", qPrintable(key), (end - start));
    }
    else
    {
      printf("%s: Never Finished\n", qPrintable(key));
    }
  }
  std::cout << "---------------  END TIMINGS ---------------" << std::endl;
}