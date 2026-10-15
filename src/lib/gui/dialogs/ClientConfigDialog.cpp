/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientConfigDialog.h"
#include "ui_ClientConfigDialog.h"

#include "common/PlatformInfo.h"
#include "common/Settings.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QPushButton>

ClientConfigDialog::ClientConfigDialog(QWidget *parent)
    : QDialog(parent),
      ui(new Ui::ClientConfigDialog),
      m_buttonBox{new SettingsDialogButtonBox(this)}
{
  ui->setupUi(this);
  layout()->addWidget(m_buttonBox);
  ui->cbMacNativeCapsLock->setVisible(deskflow::platform::isMac());
  initConnections();
  load();
  updateControls();
  setButtonBoxEnabledButtons();
}

ClientConfigDialog::~ClientConfigDialog()
{
  delete ui;
}

void ClientConfigDialog::changeEvent(QEvent *e)
{
  QDialog::changeEvent(e);
  if (e->type() == QEvent::LanguageChange)
    ui->retranslateUi(this);
}

void ClientConfigDialog::updateControls() const
{
  const auto writable = Settings::isWritable();
  ui->cbDynamicConnectTime->setEnabled(writable);
  ui->cbMacNativeCapsLock->setEnabled(writable);
  ui->cbLanguageSync->setEnabled(writable && !(deskflow::platform::isMac() && ui->cbMacNativeCapsLock->isChecked()));
  ui->cbXScrollInvert->setEnabled(writable);
  ui->sbXScrollScale->setEnabled(writable);
  ui->cbYScrollInvert->setEnabled(writable);
  ui->sbYScrollScale->setEnabled(writable);
}

void ClientConfigDialog::initConnections() const
{
  connect(m_buttonBox, &SettingsDialogButtonBox::accepted, this, &ClientConfigDialog::save);
  connect(m_buttonBox, &SettingsDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_buttonBox, &SettingsDialogButtonBox::reset, this, &ClientConfigDialog::load);
  connect(m_buttonBox, &SettingsDialogButtonBox::restoreDefault, this, &ClientConfigDialog::resetToDefault);

  connect(
      ui->cbDynamicConnectTime, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons
  );
  connect(ui->cbMacNativeCapsLock, &QCheckBox::toggled, this, [this](bool checked) {
    if (deskflow::platform::isMac() && checked)
      ui->cbLanguageSync->setChecked(false);
    updateControls();
    setButtonBoxEnabledButtons();
  });
  connect(ui->cbLanguageSync, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->cbYScrollInvert, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->sbYScrollScale, &QDoubleSpinBox::valueChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->cbXScrollInvert, &QCheckBox::checkStateChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->sbXScrollScale, &QDoubleSpinBox::valueChanged, this, &ClientConfigDialog::setButtonBoxEnabledButtons);
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &ClientConfigDialog::updateControls);
}

bool ClientConfigDialog::isModified() const
{
  return (ui->cbMacNativeCapsLock->isChecked() != Settings::value(Settings::Client::MacNativeCapsLock).toBool()) ||
         (ui->cbDynamicConnectTime->isChecked() != Settings::value(Settings::Client::DynamicConnectionRetry).toBool()
         ) ||
         (ui->cbLanguageSync->isChecked() != Settings::value(Settings::Client::LanguageSync).toBool()) ||
         (ui->cbYScrollInvert->isChecked() != Settings::value(Settings::Client::InvertYScroll).toBool()) ||
         (ui->sbYScrollScale->value() != Settings::value(Settings::Client::YScrollScale).toDouble()) ||
         (ui->cbXScrollInvert->isChecked() != Settings::value(Settings::Client::InvertXScroll).toBool()) ||
         (ui->sbXScrollScale->value() != Settings::value(Settings::Client::XScrollScale).toDouble());
}

bool ClientConfigDialog::isDefault() const
{
  return (ui->cbMacNativeCapsLock->isChecked() == Settings::defaultValue(Settings::Client::MacNativeCapsLock).toBool()
         ) &&
         (ui->cbDynamicConnectTime->isChecked() ==
          Settings::defaultValue(Settings::Client::DynamicConnectionRetry).toBool()) &&
         (ui->cbLanguageSync->isChecked() == Settings::defaultValue(Settings::Client::LanguageSync).toBool()) &&
         (ui->cbYScrollInvert->isChecked() == Settings::defaultValue(Settings::Client::InvertYScroll).toBool()) &&
         (ui->sbYScrollScale->value() == Settings::defaultValue(Settings::Client::YScrollScale).toDouble()) &&
         (ui->cbXScrollInvert->isChecked() == Settings::defaultValue(Settings::Client::InvertXScroll).toBool()) &&
         (ui->sbXScrollScale->value() == Settings::defaultValue(Settings::Client::XScrollScale).toDouble());
}

void ClientConfigDialog::setButtonBoxEnabledButtons() const
{
  const bool modified = isModified();
  m_buttonBox->enableSave(modified);
  m_buttonBox->enableReset(modified);
  m_buttonBox->enableRestoreDefaults(!isDefault());
}

void ClientConfigDialog::load()
{
  ui->cbDynamicConnectTime->setChecked(Settings::value(Settings::Client::DynamicConnectionRetry).toBool());
  ui->cbLanguageSync->setChecked(Settings::value(Settings::Client::LanguageSync).toBool());
  ui->cbMacNativeCapsLock->setChecked(Settings::value(Settings::Client::MacNativeCapsLock).toBool());
  if (deskflow::platform::isMac() && ui->cbMacNativeCapsLock->isChecked())
    ui->cbLanguageSync->setChecked(false);
  ui->cbYScrollInvert->setChecked(Settings::value(Settings::Client::InvertYScroll).toBool());
  ui->sbYScrollScale->setValue(Settings::value(Settings::Client::YScrollScale).toDouble());
  ui->cbXScrollInvert->setChecked(Settings::value(Settings::Client::InvertXScroll).toBool());
  ui->sbXScrollScale->setValue(Settings::value(Settings::Client::XScrollScale).toDouble());
}

void ClientConfigDialog::resetToDefault()
{
  ui->cbDynamicConnectTime->setChecked(Settings::defaultValue(Settings::Client::DynamicConnectionRetry).toBool());
  ui->cbLanguageSync->setChecked(Settings::defaultValue(Settings::Client::LanguageSync).toBool());
  ui->cbMacNativeCapsLock->setChecked(Settings::defaultValue(Settings::Client::MacNativeCapsLock).toBool());
  ui->cbYScrollInvert->setChecked(Settings::defaultValue(Settings::Client::InvertYScroll).toBool());
  ui->sbYScrollScale->setValue(Settings::defaultValue(Settings::Client::YScrollScale).toDouble());
  ui->cbXScrollInvert->setChecked(Settings::defaultValue(Settings::Client::InvertXScroll).toBool());
  ui->sbXScrollScale->setValue(Settings::defaultValue(Settings::Client::XScrollScale).toDouble());
}

void ClientConfigDialog::save()
{
  if (deskflow::platform::isMac())
    Settings::setValue(Settings::Client::MacNativeCapsLock, ui->cbMacNativeCapsLock->isChecked());
  Settings::setValue(Settings::Client::DynamicConnectionRetry, ui->cbDynamicConnectTime->isChecked());
  Settings::setValue(Settings::Client::LanguageSync, ui->cbLanguageSync->isChecked());
  Settings::setValue(Settings::Client::InvertYScroll, ui->cbYScrollInvert->isChecked());
  Settings::setValue(Settings::Client::YScrollScale, ui->sbYScrollScale->value());
  Settings::setValue(Settings::Client::InvertXScroll, ui->cbXScrollInvert->isChecked());
  Settings::setValue(Settings::Client::XScrollScale, ui->sbXScrollScale->value());
  QDialog::accept();
}
