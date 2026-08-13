/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QCommandLineOption>
/**
 * @brief The CoreArgs class
 * This class contains args for the CoreArgParser
 */
struct CoreArgs
{
  inline static const auto helpOption = QCommandLineOption({"h", "help"}, "Display Help on the command line");
  inline static const auto versionOption = QCommandLineOption({"v", "version"}, "Display version information");
  inline static const auto multiInstanceOption =
      QCommandLineOption("new-instance", "Skip the check for a running instance, always makes a new instance");
  inline static const auto configOption =
      QCommandLineOption({"s", "settings"}, "override configuration file to use", "configFile");
  // MouseTransfer warm-standby latch: load fully, then WAIT until this file is deleted before touching
  // input / screen / network. Lets a managed launcher pre-spawn the core during game-mode dormancy so
  // the process cold-start is paid off the resume critical path; deleting the file releases the latch.
  inline static const auto startGatedOption = QCommandLineOption(
      "start-gated", "Warm-standby: wait until <gateFile> is deleted before starting the KVM engine", "gateFile"
  );

  inline static const auto options = {
      helpOption, versionOption, multiInstanceOption, configOption, startGatedOption
  };
};
