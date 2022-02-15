#include "fitshowtreadmill.h"
#include "ios/lockscreen.h"
#include "keepawakehelper.h"
#include "virtualtreadmill.h"
#include <QBluetoothLocalDevice>
#include <QDateTime>
#include <QFile>
#include <QMetaEnum>
#include <QSettings>
#include <chrono>

using namespace std::chrono_literals;

#define BLE_SERIALOUTPUT_MAXSIZE 25

fitshowtreadmill::fitshowtreadmill(uint32_t pollDeviceTime, bool noConsole, bool noHeartService, double forceInitSpeed,
                                   double forceInitInclination) {
    Q_UNUSED(noConsole)
    this->noHeartService = noHeartService;

    if (forceInitSpeed > 0) {
        lastSpeed = forceInitSpeed;
    }

    if (forceInitInclination > 0) {
        lastInclination = forceInitInclination;
    }
#if defined(Q_OS_IOS) && !defined(IO_UNDER_QT)
    h = new lockscreen();
#endif

    refresh = new QTimer(this);
    initDone = false;
    QSettings settings;
    anyrun = settings.value(QStringLiteral("fitshow_anyrun"), false).toBool();
    truetimer = settings.value(QStringLiteral("fitshow_truetimer"), false).toBool();
    connect(refresh, &QTimer::timeout, this, &fitshowtreadmill::update);
    refresh->start(pollDeviceTime);
}

fitshowtreadmill::~fitshowtreadmill() {
    if (refresh) {
        refresh->stop();
        delete refresh;
    }
    if (virtualTreadMill) {
        delete virtualTreadMill;
    }
#if defined(Q_OS_IOS) && !defined(IO_UNDER_QT)
    if (h)
        delete h;
#endif
}

void fitshowtreadmill::scheduleWrite(const uint8_t *data, uint8_t data_len, const QString &info) {
    bufferWrite.append((char)data_len);
    bufferWrite.append(QByteArray((const char *)data, data_len));
    debugMsgs.append(info);
}

void fitshowtreadmill::writeCharacteristic(const uint8_t *data, uint8_t data_len, const QString &info) {
    QEventLoop loop;
    QTimer timeout;
    QByteArray qba((const char *)data, data_len);
    if (!info.isEmpty()) {
        emit debug(QStringLiteral(" >>") + qba.toHex(' ') + QStringLiteral(" // ") + info);
    }

    connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, &loop, &QEventLoop::quit);
    timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    gattCommunicationChannelService->writeCharacteristic(gattWriteCharacteristic, qba);

    loop.exec();

    if (timeout.isActive() == false) {
        emit debug(QStringLiteral(" exit for timeout"));
    }
}

bool fitshowtreadmill::checkIncomingPacket(const uint8_t *data, uint8_t data_len) const {
    if (data_len >= 4 && data[0] == FITSHOW_PKT_HEADER && data[data_len - 1] == FITSHOW_PKT_FOOTER) {
        int n4 = 0;
        int n5 = 1;
        int n6 = data_len - 2;

        while (true) {
            if (n5 >= n6) {
                break;
            }
            n4 ^= data[n5];
            ++n5;
        }
        return n4 == data[n6];
    } else
        return false;
}

bool fitshowtreadmill::writePayload(const uint8_t *array, uint8_t size, const QString &info) {
    if (size + 3 > BLE_SERIALOUTPUT_MAXSIZE) {
        return false;
    }
    uint8_t array2[BLE_SERIALOUTPUT_MAXSIZE];
    array2[0] = FITSHOW_PKT_HEADER;
    uint8_t n = 0, i = 0;
    while (i < size) {
        array2[i + 1] = array[i];
        n ^= array[i++];
    }
    array2[size + 1] = n;
    array2[size + 2] = FITSHOW_PKT_FOOTER;
    writeCharacteristic(array2, size + 3, info);
    return true;
}

void fitshowtreadmill::forceSpeedOrIncline(double requestSpeed, double requestIncline) {
    if (MAX_SPEED > 0) {
        requestSpeed *= 10.0;
        if (requestSpeed >= MAX_SPEED) {
            requestSpeed = MAX_SPEED;
        } else if (requestSpeed <= MIN_SPEED) {
            requestSpeed = MIN_SPEED;
        }
        if (requestIncline >= MAX_INCLINE) {
            requestIncline = MAX_INCLINE;
        } else if (requestIncline <= MIN_INCLINE) {
            requestIncline = MIN_INCLINE;
        }

        uint8_t writeIncline[] = {FITSHOW_SYS_CONTROL, FITSHOW_CONTROL_TARGET_OR_RUN, (uint8_t)(requestSpeed + 0.5),
                                  (uint8_t)requestIncline};
        scheduleWrite(writeIncline, sizeof(writeIncline),
                      QStringLiteral("forceSpeedOrIncline speed=") + QString::number(requestSpeed) +
                          QStringLiteral(" incline=") + QString::number(requestIncline));
    }
}

void fitshowtreadmill::update() {
    if (!m_control || m_control->state() == QLowEnergyController::UnconnectedState) {
        emit disconnected();
        return;
    }

    if (initRequest) {
        initRequest = false;
        btinit((lastSpeed > 0 ? true : false));
    } else if (bluetoothDevice.isValid() && m_control->state() == QLowEnergyController::DiscoveredState &&
               gattCommunicationChannelService && gattWriteCharacteristic.isValid() &&
               gattNotifyCharacteristic.isValid() && initDone) {
        QSettings settings;
        // ******************************************* virtual treadmill init *************************************
        if (!firstInit && searchStopped && !virtualTreadMill) {
            bool virtual_device_enabled = settings.value(QStringLiteral("virtual_device_enabled"), true).toBool();
            if (virtual_device_enabled) {
                emit debug(QStringLiteral("creating virtual treadmill interface..."));
                virtualTreadMill = new virtualtreadmill(this, noHeartService);
                connect(virtualTreadMill, &virtualtreadmill::debug, this, &fitshowtreadmill::debug);

                firstInit = 1;
            }
        }
        // ********************************************************************************************************

        emit debug(QStringLiteral("fitshow Treadmill RSSI ") + QString::number(bluetoothDevice.rssi()));

        update_metrics(true, watts(settings.value(QStringLiteral("weight"), 75.0).toFloat()));

        if (requestSpeed != -1) {
            if (requestSpeed != currentSpeed().value()) {
                emit debug(QStringLiteral("writing speed ") + QString::number(requestSpeed));
                double inc = currentInclination().value();
                if (requestInclination != -1) {
                    int diffInc = (int)(requestInclination - inc);
                    if (!diffInc) {
                        if (requestInclination > inc) {
                            inc += 1.0;
                        } else if (requestInclination < inc) {
                            inc -= 1.0;
                        }
                    } else {
                        inc = (int)requestInclination;
                    }
                    requestInclination = -1;
                }
                forceSpeedOrIncline(requestSpeed, inc);
            }
            requestSpeed = -1;
        }

        if (requestInclination != -1) {
            double inc = currentInclination().value();
            if (requestInclination != inc) {
                emit debug(QStringLiteral("writing incline ") + QString::number(requestInclination));
                int diffInc = (int)(requestInclination - inc);
                if (!diffInc) {
                    if (requestInclination > inc) {
                        inc += 1.0;
                    } else if (requestInclination < inc) {
                        inc -= 1.0;
                    }
                } else {
                    inc = (int)requestInclination;
                }
                double speed = currentSpeed().value();
                if (requestSpeed != -1) {
                    speed = requestSpeed;
                    requestSpeed = -1;
                }
                forceSpeedOrIncline(speed, inc);
            }
            requestInclination = -1;
        }
        if (requestStart != -1) {
            emit debug(QStringLiteral("starting..."));
            if (lastSpeed == 0.0) {
                lastSpeed = 0.5;
            }
            btinit(true);
            lastStart = QDateTime::currentMSecsSinceEpoch();
            requestStart = -1;
            emit tapeStarted();
        }
        if (requestStop != -1) {
            if (paused) {
                lastStop = QDateTime::currentMSecsSinceEpoch();
                uint8_t pauseTape[] = {FITSHOW_SYS_CONTROL, FITSHOW_CONTROL_PAUSE}; // to verify
                emit debug(QStringLiteral("pausing..."));
                scheduleWrite(pauseTape, sizeof(pauseTape), QStringLiteral("pause tape"));
            } else {
                uint8_t stopTape[] = {FITSHOW_SYS_CONTROL, FITSHOW_CONTROL_STOP};
                emit debug(QStringLiteral("stopping..."));
                lastStop = QDateTime::currentMSecsSinceEpoch();
                scheduleWrite(stopTape, sizeof(stopTape), QStringLiteral("stop tape"));
            }
            requestStop = -1;
        }

        if (retrySend >= 6) { // 3 retries
            emit debug(QStringLiteral("WARNING: answer not received for command "
                                      "%1 / %2 (%3)")
                           .arg(((uint8_t)bufferWrite.at(1)), 2, 16, QChar('0'))
                           .arg(((uint8_t)bufferWrite.at(2)), 2, 16, QChar('0'))
                           .arg(debugMsgs.at(0)));
            removeFromBuffer();
        }
        if (!bufferWrite.isEmpty()) {
            retrySend++;
            if (retrySend % 2) { // retry only on odd values: on even values wait some more time for response
                const uint8_t *write_pld = (const uint8_t *)bufferWrite.constData();
                writePayload(write_pld + 1, write_pld[0], debugMsgs.at(0));
            }
        } else {
            uint8_t status = FITSHOW_SYS_STATUS;
            writePayload(&status, 1);
        }
    }
}

void fitshowtreadmill::removeFromBuffer() {
    if (!bufferWrite.isEmpty()) {
        bufferWrite.remove(0, ((uint8_t)bufferWrite.at(0)) + 1);
    }
    if (!debugMsgs.isEmpty()) {
        debugMsgs.removeFirst();
    }
    retrySend = 0;
}

void fitshowtreadmill::serviceDiscovered(const QBluetoothUuid &gatt) {
    uint32_t servRepr = gatt.toUInt32();
    emit debug(QStringLiteral("serviceDiscovered ") + gatt.toString() + QStringLiteral(" ") +
               QString::number(servRepr));
    if (servRepr == 0xfff0 || (servRepr == 0xffe0 && serviceId.isNull())) {
        serviceId = gatt; // NOTE: clazy-rule-of-tow
    }
}

void fitshowtreadmill::sendSportData() {
    if (!anyrun) {
        uint8_t writeSport[] = {FITSHOW_SYS_DATA, FITSHOW_DATA_SPORT};
        scheduleWrite(writeSport, sizeof(writeSport), QStringLiteral("SendSportsData"));
    }
}

void fitshowtreadmill::characteristicChanged(const QLowEnergyCharacteristic &characteristic,
                                             const QByteArray &newValue) {
    // qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    QSettings settings;
    QString heartRateBeltName =
        settings.value(QStringLiteral("heart_rate_belt_name"), QStringLiteral("Disabled")).toString();
    Q_UNUSED(characteristic);
    QByteArray value = newValue;

    emit debug(QStringLiteral(" << ") + QString::number(value.length()) + QStringLiteral(" ") + value.toHex(' '));

    emit debug(QStringLiteral("packetReceived!"));
    emit packetReceived();

    lastPacket = value;
    const uint8_t *full_array = (uint8_t *)value.constData();
    uint8_t full_len = value.length();
    if (!checkIncomingPacket(full_array, full_len)) {
        emit debug(QStringLiteral("Invalid packet"));
        return;
    }
    const uint8_t *array = full_array + 1;
    const uint8_t cmd = array[0];
    const uint8_t par = array[1];

    const uint8_t *array_expected = (const uint8_t *)bufferWrite.constData() + 1;
    uint8_t len = full_len - 3;
    if (cmd != FITSHOW_SYS_STATUS && !bufferWrite.isEmpty() && *array_expected == cmd && *(array_expected + 1) == par) {
        removeFromBuffer();
    }
    if (cmd == FITSHOW_SYS_INFO) {
        if (par == FITSHOW_INFO_SPEED) {
            if (full_len > 6) {
                MAX_SPEED = full_array[3];
                MIN_SPEED = full_array[4];
                emit debug(QStringLiteral("Speed between ") + QString::number(MIN_SPEED) + QStringLiteral(" and ") +
                           QString::number(MAX_SPEED));
                if (full_len > 7) {
                    UNIT = full_array[5];
                }
            }
        } else if (par == FITSHOW_INFO_UNKNOWN) {
            if (full_len >= 9) {
                MAX_SPEED = full_array[3];
                MIN_SPEED = full_array[4];
                MAX_INCLINE = full_array[5];
                MIN_INCLINE = full_array[6];
                IS_HRC = (bool)full_array[7];
                COUNTDOWN_VALUE = full_array[8];
            }
        } else if (par == FITSHOW_INFO_INCLINE) {
            if (full_len < 7) {
                MAX_INCLINE = 0;
                emit debug(QStringLiteral("Incline not supported"));
            } else {
                MAX_INCLINE = full_array[3];
                MIN_INCLINE = full_array[4];
                if (full_len > 7 && (full_array[5] & 0x2) != 0x0) {
                    IS_PAUSE = true;
                }
                emit debug(QStringLiteral("Incline between ") + QString::number(MIN_INCLINE) + QStringLiteral(" and ") +
                           QString::number(MAX_INCLINE));
            }
        } else if (par == FITSHOW_INFO_MODEL) {
            if (full_len > 7) {
                uint16_t second = (full_array[5] << 8) | full_array[4];
                DEVICE_ID_NAME = QStringLiteral("%1-%2")
                                     .arg(full_array[3], 2, 16, QLatin1Char('0'))
                                     .arg(second, 4, 16, QLatin1Char('0'));
                emit debug(QStringLiteral("DEVICE ") + DEVICE_ID_NAME);
            }
        } else if (par == FITSHOW_INFO_TOTAL) {
            if (full_len > 8) {
                TOTAL = (full_array[6] << 24 | full_array[5] << 16 | full_array[4] << 8 | full_array[3]);
                emit debug(QStringLiteral("TOTAL ") + QString::number(TOTAL));
            } else {
                TOTAL = -1;
            }
        } else if (par == FITSHOW_INFO_DATE) {
            if (full_len > 7) {
                FACTORY_DATE = QDate(full_array[3] + 2000, full_array[4], full_array[5]);
                emit debug(QStringLiteral("DATE ") + FACTORY_DATE.toString());
            } else {
                FACTORY_DATE = QDate();
            }
        }
    } else if (cmd == FITSHOW_SYS_CONTROL) {
        SYS_CONTROL_CMD = par;
        emit debug(QStringLiteral("SYS_CONTROL received ok: par ") + QString::number(par));
        if (par == FITSHOW_CONTROL_TARGET_OR_RUN) {
            QString dbg;
            if (full_len > 5) {
                dbg = QStringLiteral("Actual speed ") + QString::number(full_array[3] / 10.0);
                if (full_len > 6) {
                    dbg += QStringLiteral("; actual incline: ") + QString::number(full_array[4]);
                }
            }
            emit debug(dbg);
        }
    }
    if (cmd == FITSHOW_SYS_STATUS) {
        CURRENT_STATUS = par;
        emit debug(QStringLiteral("STATUS ") + QString::number(par));
        if (par == FITSHOW_STATUS_START) {
            if (len > 2) {
                COUNTDOWN_VALUE = array[2];
                emit debug(QStringLiteral("CONTDOWN ") + QString::number(COUNTDOWN_VALUE));
            }
        } else if (par == FITSHOW_STATUS_RUNNING || par == FITSHOW_STATUS_STOP || par == FITSHOW_STATUS_PAUSED ||
                   par == FITSHOW_STATUS_END) {
            if (full_len >= 17) {
                if (par == FITSHOW_STATUS_RUNNING)
                    IS_RUNNING = true;
                else {
                    IS_STATUS_STUDY = false;
                    IS_STATUS_ERRO = false;
                    IS_STATUS_SAFETY = false;
                    IS_RUNNING = false;
                }

                double speed = array[2] / 10.0;
                double incline = array[3];
                uint16_t seconds_elapsed = anyrun ? array[4] * 60 + array[5] : array[4] | array[5] << 8;
                double distance = (anyrun ? (array[7] | array[6] << 8) : (array[6] | array[7] << 8)) / 10.0;
                double kcal = anyrun ? (array[9] | array[8] << 8) : (array[8] | array[9] << 8);
                uint16_t step_count = anyrun ? (array[11] | array[10] << 8) : (array[10] | array[11] << 8);
                // final byte b2 = array[13]; Mark_zuli???
                double heart = array[12];

                if (MAX_INCLINE == 0) {
                    qDebug() << QStringLiteral("inclination out of range, resetting it to 0...") << incline;
                    incline = 0;
                }

                if (!firstCharacteristicChanged) {
                    DistanceCalculated +=
                        ((speed / 3600.0) /
                         (1000.0 / (lastTimeCharacteristicChanged.msecsTo(QDateTime::currentDateTime()))));
                }

                emit debug(QStringLiteral("Current elapsed from treadmill: ") + QString::number(seconds_elapsed));
                emit debug(QStringLiteral("Current speed: ") + QString::number(speed));
                emit debug(QStringLiteral("Current incline: ") + QString::number(incline));
                emit debug(QStringLiteral("Current heart: ") + QString::number(heart));
                emit debug(QStringLiteral("Current Distance: ") + QString::number(distance));
                emit debug(QStringLiteral("Current Distance Calculated: ") + QString::number(DistanceCalculated));
                emit debug(QStringLiteral("Current KCal: ") + QString::number(kcal));
                emit debug(QStringLiteral("Current step countl: ") + QString::number(step_count));

                if (m_control->error() != QLowEnergyController::NoError) {
                    qDebug() << QStringLiteral("QLowEnergyController ERROR!!") << m_control->errorString();
                }

                if (speed > 0) {
                    lastStart =
                        0; // telling to the UI that it could be autostoppable when the speed it will reach again 0
                } else {
                    lastStop = 0;
                }

                if (Speed.value() != speed) {
                    Speed = speed;
                    emit speedChanged(speed);
                }
                if (Inclination.value() != incline) {
                    Inclination = incline;
                    emit inclinationChanged(0, incline);
                }

                KCal = kcal;
                if (truetimer)
                    elapsed = seconds_elapsed;
                Distance = distance;
#ifdef Q_OS_ANDROID
                if (settings.value("ant_heart", false).toBool())
                    Heart = (uint8_t)KeepAwakeHelper::heart();
                else
#endif
                {
                    if (heartRateBeltName.startsWith(QStringLiteral("Disabled"))) {
#if defined(Q_OS_IOS) && !defined(IO_UNDER_QT)
                        long appleWatchHeartRate = h->heartRate();
                        h->setKcal(KCal.value());
                        h->setDistance(Distance.value());
                        Heart = appleWatchHeartRate;
                        debug("Current Heart from Apple Watch: " + QString::number(appleWatchHeartRate));
#else
                        Heart = heart;
#endif
                    }
                }

                if (speed > 0) {
                    lastSpeed = speed;
                    lastInclination = incline;
                }

                lastTimeCharacteristicChanged = QDateTime::currentDateTime();
                firstCharacteristicChanged = false;
                if (par != FITSHOW_STATUS_RUNNING) {
                    sendSportData();
                }
            }
        } else {
            if (par == FITSHOW_STATUS_NORMAL) {
                sendSportData();
                IS_STATUS_STUDY = false;
                IS_STATUS_ERRO = false;
                IS_STATUS_SAFETY = false;
                IS_RUNNING = false;
            } else if (par == FITSHOW_STATUS_STUDY) {
                IS_STATUS_STUDY = true;
            } else if (par == FITSHOW_STATUS_ERROR) {
                if (len > 2) {
                    IS_STATUS_ERRO = true;
                    ERRNO = array[2];
                    sendSportData();
                }
            } else if (par == FITSHOW_STATUS_SAFETY) {
                ERRNO = 100;
                IS_STATUS_SAFETY = true;
                sendSportData();
            }
            if (Speed.value() != 0.0) {
                Speed = 0.0;
                emit speedChanged(0.0);
            }
            if (Inclination.value() != 0.0) {
                Inclination = 0.0;
                emit inclinationChanged(0.0, 0.0);
            }
        }
    } else if (cmd == FITSHOW_SYS_DATA) {
        if (par == FITSHOW_DATA_INFO) {
            if (len > 13) {
                SPORT_ID = array[6] | array[7] << 8 | array[8] << 16 | array[9] << 24;
                USER_ID = array[2] | array[3] << 8 | array[4] << 16 | array[5] << 24;
                RUN_WAY = array[10];
                int indoorrun_TIME_DATA = array[12] | array[13] << 8;
                if (RUN_WAY == FITSHOW_SYS_MODE_TIMER) {
                    INDOORRUN_MODE = 2;
                    INDOORRUN_TIME_DATA = indoorrun_TIME_DATA;
                } else if (RUN_WAY == FITSHOW_SYS_MODE_DISTANCE) {
                    INDOORRUN_MODE = 1;
                    INDOORRUN_DISTANCE_DATA = indoorrun_TIME_DATA;
                } else if (RUN_WAY == FITSHOW_SYS_MODE_CALORIE) {
                    INDOORRUN_MODE = 3;
                    INDOORRUN_CALORIE_DATA = indoorrun_TIME_DATA / 10;
                } else if (RUN_WAY == FITSHOW_SYS_MODE_PROGRAMS) {
                    INDOORRUN_MODE = 4;
                    INDOORRUN_TIME_DATA = indoorrun_TIME_DATA;
                    INDOORRUN_PARAM_NUM = array[11];
                } else {
                    INDOORRUN_MODE = 0;
                }
                emit debug(QStringLiteral("USER_ID = %1").arg(USER_ID));
                emit debug(QStringLiteral("SPORT_ID = %1").arg(SPORT_ID));
                emit debug(QStringLiteral("RUN_WAY = %1").arg(RUN_WAY));
                emit debug(QStringLiteral("INDOORRUN_MODE = %1").arg(INDOORRUN_MODE));
                emit debug(QStringLiteral("INDOORRUN_TIME_DATA = %1").arg(INDOORRUN_TIME_DATA));
                emit debug(QStringLiteral("INDOORRUN_PARAM_NUM = %1").arg(INDOORRUN_PARAM_NUM));
                emit debug(QStringLiteral("INDOORRUN_CALORIE_DATA = %1").arg(INDOORRUN_CALORIE_DATA));
                emit debug(QStringLiteral("INDOORRUN_DISTANCE_DATA = %1").arg(INDOORRUN_DISTANCE_DATA));
            }
        } else if (par == FITSHOW_DATA_SPORT) {
            if (len > 9) {
                double kcal = array[6] | array[7] << 8;
                uint16_t seconds_elapsed = array[2] | array[3] << 8;
                double distance = array[4] | array[5] << 8;
                uint16_t step_count = array[8] | array[9] << 8;

                emit debug(QStringLiteral("Current elapsed from treadmill: ") + QString::number(seconds_elapsed));
                emit debug(QStringLiteral("Current step countl: ") + QString::number(step_count));
                emit debug(QStringLiteral("Current KCal: ") + QString::number(kcal));
                emit debug(QStringLiteral("Current Distance: ") + QString::number(distance));
                KCal = kcal;
                if (truetimer)
                    elapsed = seconds_elapsed;
                Distance = distance;
            }
        }
    }
}

void fitshowtreadmill::btinit(bool startTape) {
    uint8_t initInfos[] = {FITSHOW_INFO_SPEED, FITSHOW_INFO_INCLINE, FITSHOW_INFO_TOTAL, FITSHOW_INFO_DATE};
    uint8_t initDataStart1[] = {FITSHOW_SYS_INFO, FITSHOW_INFO_UNKNOWN};

    QDateTime now = QDateTime::currentDateTime();
    uint8_t initDataStart0[] = {FITSHOW_SYS_INFO,
                                FITSHOW_INFO_MODEL,
                                (uint8_t)(now.date().year() - 2000),
                                (uint8_t)(now.date().month()),
                                (uint8_t)(now.date().day()),
                                (uint8_t)(now.time().hour()),
                                (uint8_t)(now.time().minute()),
                                (uint8_t)(now.time().second())};

    uint8_t startTape1[] = {
        FITSHOW_SYS_CONTROL,
        FITSHOW_CONTROL_READY_OR_START,
        (FITSHOW_TREADMILL_SPORT_ID >> 0) & 0xFF,
        (FITSHOW_TREADMILL_SPORT_ID >> 8) & 0xFF,
        (FITSHOW_TREADMILL_SPORT_ID >> 16) & 0xFF,
        (FITSHOW_TREADMILL_SPORT_ID >> 24) & 0xFF,
        FITSHOW_SYS_MODE_NORMAL,
        0x00, // number of blocks (u8)
        0x00,
        0x00 // mode-dependent value (u16le)
    };       // to verify
    QSettings settings;
    int user_id = settings.value(QStringLiteral("fitshow_user_id"), 0x13AA).toInt();
    uint8_t weight = (uint8_t)(settings.value(QStringLiteral("weight"), 75.0).toFloat() + 0.5);
    uint8_t initUserData[] = {FITSHOW_SYS_CONTROL, FITSHOW_CONTROL_USER, 0, 0, 0, 0, 0, 0};
    initUserData[2] = (user_id >> 0) & 0xFF;
    initUserData[3] = (user_id >> 8) & 0xFF;
    initUserData[4] = 110;
    initUserData[5] = 30; // age
    initUserData[6] = weight;
    initUserData[7] = 170; // height
    scheduleWrite(initUserData, sizeof(initUserData) - (!anyrun ? 1 : 0), QStringLiteral("init_user"));
    if (!anyrun)
        scheduleWrite(initDataStart0, sizeof(initDataStart0),
                      QStringLiteral("init ") + QString::number(initDataStart0[1]));
    for (uint8_t i = 0; i < sizeof(initInfos); i++) {
        if (!anyrun)
            initDataStart1[1] = initInfos[i];
        scheduleWrite(initDataStart1, sizeof(initDataStart1),
                      QStringLiteral("init ") + QString::number(initDataStart1[1]));
        if (anyrun)
            break;
    }

    if (startTape) {
        scheduleWrite(startTape1, sizeof(startTape1), QStringLiteral("init_start"));
        forceSpeedOrIncline(lastSpeed, lastInclination);
    }

    // initUserData[0] = FITSHOW_SYS_DATA;
    // initUserData[1] = FITSHOW_DATA_INFO;
    // scheduleWrite(initUserData, sizeof(initUserData) - 2, QStringLiteral("check what is going on for given user"));

    initDone = true;
}

void fitshowtreadmill::stateChanged(QLowEnergyService::ServiceState state) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceState>();
    emit debug(QStringLiteral("BTLE stateChanged ") + QString::fromLocal8Bit(metaEnum.valueToKey(state)));
    if (state == QLowEnergyService::ServiceDiscovered) {
        uint32_t id32;
        auto characteristics_list = gattCommunicationChannelService->characteristics();
        for (const QLowEnergyCharacteristic &c : qAsConst(characteristics_list)) {
            qDebug() << QStringLiteral("c -> ") << c.uuid();
            id32 = c.uuid().toUInt32();
            auto descriptors_list = c.descriptors();
            for (const QLowEnergyDescriptor &d : qAsConst(descriptors_list)) {
                qDebug() << QStringLiteral("d -> ") << d.uuid();
            }
            if (id32 == 0xffe1 || id32 == 0xfff2) {
                gattWriteCharacteristic = c;
            } else if (id32 == 0xffe4 || id32 == 0xfff1) {
                gattNotifyCharacteristic = c;
            }
        }

        if (!gattWriteCharacteristic.isValid()) {
            qDebug() << QStringLiteral("gattWriteCharacteristic not valid");
            return;
        }
        if (!gattNotifyCharacteristic.isValid()) {
            qDebug() << QStringLiteral("gattNotifyCharacteristic not valid");
            return;
        }

        // establish hook into notifications
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicChanged, this,
                &fitshowtreadmill::characteristicChanged);
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, this,
                &fitshowtreadmill::characteristicWritten);
        connect(gattCommunicationChannelService,
                static_cast<void (QLowEnergyService::*)(QLowEnergyService::ServiceError)>(&QLowEnergyService::error),
                this, &fitshowtreadmill::errorService);
        connect(gattCommunicationChannelService, &QLowEnergyService::descriptorWritten, this,
                &fitshowtreadmill::descriptorWritten);

        QByteArray descriptor;
        descriptor.append((char)0x01);
        descriptor.append((char)0x00);
        gattCommunicationChannelService->writeDescriptor(
            gattNotifyCharacteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
    }
}

void fitshowtreadmill::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    emit debug(QStringLiteral("descriptorWritten ") + descriptor.name() + QStringLiteral(" ") + newValue.toHex(' '));

    initRequest = true;
    emit connectedAndDiscovered();
}

void fitshowtreadmill::characteristicWritten(const QLowEnergyCharacteristic &characteristic,
                                             const QByteArray &newValue) {
    Q_UNUSED(characteristic);
    emit debug(QStringLiteral("characteristicWritten ") + newValue.toHex(' '));
}

void fitshowtreadmill::serviceScanDone(void) {
    emit debug(QStringLiteral("serviceScanDone"));

    gattCommunicationChannelService = m_control->createServiceObject(serviceId);
    connect(gattCommunicationChannelService, &QLowEnergyService::stateChanged, this, &fitshowtreadmill::stateChanged);
#ifdef _MSC_VER
    // QTBluetooth bug on Win10 (https://bugreports.qt.io/browse/QTBUG-78488)
    QTimer::singleShot(0, [=]() { gattCommunicationChannelService->discoverDetails(); });
#else
    gattCommunicationChannelService->discoverDetails();
#endif
}

void fitshowtreadmill::errorService(QLowEnergyService::ServiceError err) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    emit debug(QStringLiteral("fitshowtreadmill::errorService ") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void fitshowtreadmill::error(QLowEnergyController::Error err) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    emit debug(QStringLiteral("fitshowtreadmill::error ") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void fitshowtreadmill::deviceDiscovered(const QBluetoothDeviceInfo &device) {
    emit debug(QStringLiteral("Found new device: ") + device.name() + QStringLiteral(" (") +
               device.address().toString() + ')');
    /*if (device.name().startsWith(QStringLiteral("FS-")) ||
        (device.name().startsWith(QStringLiteral("SW")) && device.name().length() == 14))*/
    {
        bluetoothDevice = device;
        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, &QLowEnergyController::serviceDiscovered, this, &fitshowtreadmill::serviceDiscovered);
        connect(m_control, &QLowEnergyController::discoveryFinished, this, &fitshowtreadmill::serviceScanDone);
        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, &fitshowtreadmill::error);
        connect(m_control, &QLowEnergyController::stateChanged, this, &fitshowtreadmill::controllerStateChanged);

        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, [this](QLowEnergyController::Error error) {
                    Q_UNUSED(error);
                    Q_UNUSED(this);
                    emit debug(QStringLiteral("Cannot connect to remote device."));
                    searchStopped = false;
                    emit disconnected();
                });
        connect(m_control, &QLowEnergyController::connected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("Controller connected. Search services..."));
            m_control->discoverServices();
        });
        connect(m_control, &QLowEnergyController::disconnected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("LowEnergy controller disconnected"));
            searchStopped = false;
            emit disconnected();
        });

        // Connect
        m_control->connectToDevice();
        return;
    }
}

bool fitshowtreadmill::connected() {
    if (!m_control) {
        return false;
    }
    return m_control->state() == QLowEnergyController::DiscoveredState;
}

void *fitshowtreadmill::VirtualTreadMill() { return virtualTreadMill; }

void *fitshowtreadmill::VirtualDevice() { return VirtualTreadMill(); }

void fitshowtreadmill::searchingStop() { searchStopped = true; }

void fitshowtreadmill::controllerStateChanged(QLowEnergyController::ControllerState state) {
    qDebug() << QStringLiteral("controllerStateChanged") << state;
    if (state == QLowEnergyController::UnconnectedState && m_control) {
        qDebug() << QStringLiteral("trying to connect back again...");
        initDone = false;
        m_control->connectToDevice();
    }
}

bool fitshowtreadmill::autoPauseWhenSpeedIsZero() {
    if (lastStart == 0 || QDateTime::currentMSecsSinceEpoch() > (lastStart + 10000))
        return true;
    else
        return false;
}

bool fitshowtreadmill::autoStartWhenSpeedIsGreaterThenZero() {
    if ((lastStop == 0 || QDateTime::currentMSecsSinceEpoch() > (lastStop + 25000)) && requestStop == -1)
        return true;
    else
        return false;
}
