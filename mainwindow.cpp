#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileDialog>
#include "flashingthread.h"
#include "aboutdialog.h"
#include "config.h"
#include <QDesktopServices>
#include <QMessageBox>
#include <QThread>

void MainWindow::initLibUsb()
{
    this->ui->firmwareComboBox->addItem("Custom Firmware", "custom");
    this->ui->flashButton->setEnabled(true);
    int err = libusb_init(&usbContext);
    if (err < 0) {
        // TODO: Error Message
    }
    // libusb_set_debug(usbContext, 3);
    this->pinecilCheckTimer = new QTimer(this);
    connect(this->pinecilCheckTimer, &QTimer::timeout, this, &MainWindow::updatePinecilStatus);
    this->updatePinecilStatus();
    this->pinecilCheckTimer->start(1000);
    ui->firmwareComboBox->setEnabled(true);
}

void MainWindow::ConsolePrint(const QString &text)
{
    ui->consoleTextEdit->append(text);
}

void MainWindow::ConsolePrintError(const QString &text)
{
    ui->consoleTextEdit->append("<span style='color: red;font-weight: bold;'>" + text + "</span>");
}

void MainWindow::ConsolePrintWarning(const QString &text)
{
    ui->consoleTextEdit->append("<span style='color: yellow;'>" + text + "</span>");
}

void MainWindow::ConsolePrintInfo(const QString &text)
{
    ui->consoleTextEdit->append("<span style='color: blue;'>" + text + "</span>");
}

void MainWindow::ConsolePrintSuccess(const QString &text)
{
    ui->consoleTextEdit->append("<span style='color: green;font-weight: bold;'>" + text + "</span>");
}

void MainWindow::Flash()
{
    this->ui->flashButton->setEnabled(false);
    auto flashFunc = [this](QString firmwarePath, bool deleteFirmware = false, bool massErase = false) {
        FlashingThread *workerThread = new FlashingThread(this, this->pinecilConnectionStatus == PinecilConnectionStatusEnum::ConnectedNoDriver, firmwarePath, massErase);
        connect(workerThread, &FlashingThread::consoleData, this, [this](QString data) {
            this->ConsolePrint(data);
        });
        connect(workerThread, &FlashingThread::consoleErrorData, this, [this](QString data) {
            if (data.contains("Invalid DFU suffix signature", Qt::CaseInsensitive)) {
                return;
            }
            this->ConsolePrintError(data);
        });
        connect(workerThread, &FlashingThread::successed, this, [this, deleteFirmware, firmwarePath]() {
            this->ConsolePrintSuccess("Your " + Config::deviceName + " was flashed successfully! You can disconnect it safely.");
            this->ui->flashButton->setEnabled(true);
            if (deleteFirmware) QFile::remove(firmwarePath);
        });
        connect(workerThread, &FlashingThread::failed, this, [this, deleteFirmware, firmwarePath]() {
            this->ConsolePrintError("Flashing failed.");
            this->ui->flashButton->setEnabled(true);
            if (deleteFirmware) QFile::remove(firmwarePath);
        });
        connect(workerThread, &FlashingThread::finished, workerThread, &QObject::deleteLater);
        workerThread->start();
    };
    QString firmware = ui->firmwareComboBox->currentData().toString();
    bool massErase = this->ui->massEraseCheckBox->isChecked();
    if (firmware == "custom") {
        flashFunc(ui->firmwarePathLineBox->text(), false, massErase);
    } else {
        this->ConsolePrint("Downloading firmware...\n");
        QFileInfo fi(firmware);
        QString firmwarePath = QDir(tempDir.path()).filePath(fi.fileName());
        QFile* binary = new QFile(firmwarePath);
        binary->open(QIODevice::WriteOnly);
        QNetworkReply* reply = this->networkMgr->get(QNetworkRequest(QUrl(Config::firmwareFolder + firmware)));
        connect(reply, &QNetworkReply::readyRead, [binary, reply] {
           binary->write(reply->read(reply->bytesAvailable()));
        });
        connect(reply, &QNetworkReply::finished, [fi, binary, reply, firmware, flashFunc, firmwarePath, massErase] {
            binary->close();
            binary->deleteLater();
            reply->deleteLater();
            flashFunc(firmwarePath, true, massErase);
        });
    }

}


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , pinecilConnectionStatus(PinecilConnectionStatusEnum::Disconnected)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    this->ConsolePrintInfo("<span style='color: blue;'>Pinecil Firmware Updater v1.3</span>");
    this->ConsolePrint("Looking for firmwares and latest version...");

    this->networkMgr = new QNetworkAccessManager(this);
    QNetworkReply* reply = this->networkMgr->get(QNetworkRequest(QUrl(Config::firmwareInfo)));
    connect(reply, &QNetworkReply::finished, [this, reply]() {
        QJsonParseError jsonErr;
        QJsonDocument jsonDoc = QJsonDocument::fromJson(((QString)reply->readAll()).toUtf8(), &jsonErr);
        QJsonObject json = jsonDoc.object();
        for (auto firmwareValue : json["firmwares"].toArray()) {
            QJsonObject firmware = firmwareValue.toObject();
            ui->firmwareComboBox->addItem(firmware["name"].toString(), firmware["file"].toString());
        }
        if (json["latest_version"].toString().toDouble() > 1.3) {
            QMessageBox msgBox;
            msgBox.setText("New version of Pinecil Firmware Updater was found.");
            msgBox.setInformativeText("Do you want to download it?");
            msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            msgBox.setDefaultButton(QMessageBox::Yes);
            if (msgBox.exec() == QMessageBox::Yes) {
                QDesktopServices::openUrl(QUrl(Config::updaterUrl));
            }
        }
        reply->deleteLater();
        this->initLibUsb();
    });
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    connect(reply, &QNetworkReply::errorOccurred, [this] {
#else
    connect(reply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error), [this] {
#endif
        this->ConsolePrintWarning("Failed to fetch firmwares.");
        this->initLibUsb();
    });
}

MainWindow::~MainWindow()
{
    delete this->networkMgr;
    delete this->pinecilCheckTimer;
    delete ui;
}

void MainWindow::updatePinecilStatus()
{
    int err;
    bool statusChanged = false;
    libusb_device **devices;
    int devicesCount = libusb_get_device_list(usbContext, &devices);
    if (this->previousDevicesCount != devicesCount) {
        this->previousDevicesCount = devicesCount;
        this->pinecilConnectionStatus = PinecilConnectionStatusEnum::Disconnected;
        for (int i = 0; i < devicesCount; i++) {
            libusb_device_descriptor desc;
            err = libusb_get_device_descriptor(devices[i], &desc);
            if (err == 0 && desc.idVendor == Config::dfuVID && desc.idProduct == Config::dfuPID) {
                libusb_device_handle* devHandle;
                int resp = libusb_open(devices[i], &devHandle);
                if (resp == LIBUSB_ERROR_NOT_SUPPORTED) {
                    this->pinecilConnectionStatus = PinecilConnectionStatusEnum::ConnectedNoDriver;
                } else if (resp == LIBUSB_SUCCESS) {
                    libusb_close(devHandle);
                    this->pinecilConnectionStatus = PinecilConnectionStatusEnum::Connected;
                    break;
                } else {
                    this->pinecilConnectionStatus = PinecilConnectionStatusEnum::Error;
                }
            }
        }
        statusChanged = true;
    }

    if (statusChanged) {
        switch (this->pinecilConnectionStatus.load()) {
        case PinecilConnectionStatusEnum::ConnectedNoDriver:
            ui->statusBar->showMessage(Config::deviceName + " is connected (no WinUSB)");
            break;
        case PinecilConnectionStatusEnum::Error:
            ui->statusBar->showMessage(Config::deviceName + " is connected but there is communication error.");
            break;
        case PinecilConnectionStatusEnum::Disconnected:
            ui->statusBar->showMessage(Config::deviceName + " is not connected.");
            break;
        case PinecilConnectionStatusEnum::Connected:
            ui->statusBar->showMessage(Config::deviceName + " is connected.");
            break;
        }
        if (this->flashingPending && (this->pinecilConnectionStatus == PinecilConnectionStatusEnum::ConnectedNoDriver || this->pinecilConnectionStatus == PinecilConnectionStatusEnum::Connected)) {
            this->flashPendingDialog->close();
            this->flashingPending = false;
            this->Flash();
        }
    }
}


void MainWindow::on_actionExit_triggered()
{
    QApplication::quit();
}

void MainWindow::on_firmwareComboBox_currentIndexChanged(int index)
{
    bool isCustomFirmware = ui->firmwareComboBox->itemData(index) == "custom";
    ui->firmwarePathLineBox->setEnabled(isCustomFirmware);
    ui->firmwareBrowseButton->setEnabled(isCustomFirmware);
}

void MainWindow::on_firmwareBrowseButton_clicked()
{
    QString firmwarePath = QFileDialog::getOpenFileName(this, tr("Open Firmware"), QString(), tr("Firmware files (*.bin)"));
    ui->firmwarePathLineBox->setText(firmwarePath);
}

void MainWindow::on_flashButton_clicked()
{
    if (this->pinecilConnectionStatus != PinecilConnectionStatusEnum::Connected && this->pinecilConnectionStatus != PinecilConnectionStatusEnum::ConnectedNoDriver) {
        this->flashingPending = true;
        this->flashPendingDialog = new ConnectPinecilDialog(this);
        this->flashPendingDialog->show();
        this->flashPendingDialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(this->flashPendingDialog, &ConnectPinecilDialog::rejected, [this] {
           this->flashingPending = false;
           this->flashPendingDialog = nullptr;
        });
    } else {
        this->Flash();
    }
}

void MainWindow::on_actionAbout_triggered()
{
    AboutDialog* dialog = new AboutDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}
