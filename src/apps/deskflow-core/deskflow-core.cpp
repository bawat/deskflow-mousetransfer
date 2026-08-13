/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "CoreArgParser.h"

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "deskflow/ClientApp.h"
#include "deskflow/ServerApp.h"

#if SYSAPI_WIN32
#include "arch/win32/ArchMiscWindows.h"
#include <QCoreApplication>
#endif

#include <QFileInfo>
#include <QSharedMemory>
#include <QTextStream>

void showHelp(const CoreArgParser &parser)
{
  QTextStream(stdout) << parser.helpText();
}

int main(int argc, char **argv)
{
#if SYSAPI_WIN32
  // HACK to make sure settings gets the correct qApp path
  QCoreApplication m(argc, argv);
  m.deleteLater();

  ArchMiscWindows::setInstanceWin32(GetModuleHandle(nullptr));
#endif

  Arch arch;
  arch.init();

  Log log;

  QStringList args;
  for (int i = 0; i < argc; i++)
    args.append(argv[i]);

  CoreArgParser parser(args);

  // Print any parser errors
  if (!parser.errorText().isEmpty()) {
    QTextStream(stdout) << parser.errorText() << "\n";
  }

  if (parser.help()) {
    showHelp(parser);
    return s_exitSuccess;
  }

  if (parser.version()) {
    QTextStream(stdout) << parser.versionText();
    return s_exitSuccess;
  }

  // Before we check any more args we need to check for a duplicate process.
  // Create a shared memory segment with a unique key
  // This is to prevent a new instance from running if one is already running
  QSharedMemory sharedMemory(kCoreBinName);

  // Attempt to attach first and detach in order to clean up stale shm chunks
  // This can happen if the previous instance was killed or crashed
  if (sharedMemory.attach())
    sharedMemory.detach();

  if (!sharedMemory.create(1) && parser.singleInstanceOnly()) {
    LOG_WARN("an instance of deskflow core is already running");
    return s_exitDuplicate;
  }

  parser.parse();

  // MouseTransfer warm-standby latch. When --start-gated <file> is given, the process is by now FULLY
  // loaded (DLLs mapped, QCoreApplication constructed, args parsed — the ~0.7s cold-start is done) but
  // must not touch input / screen / network yet. The managed launcher spawns us at game-mode ENTER so
  // that cold-start is paid while the user is already gaming (off the resume critical path); on tab-out
  // it DELETES the gate file and we fall straight through into the KVM engine in ~10ms. Held == file
  // exists; deletion is the atomic release. A blocked poll here is silent (no hooks, no NIC). A generous
  // cap prevents a forever-hang if the launcher dies — we exit so it can respawn a fresh core instead.
  if (const QString gateFile = parser.gateFile(); !gateFile.isEmpty()) {
    LOG_INFO("MT-gate: warm-standby loaded; waiting for release (%s)", qPrintable(gateFile));
    const double gateStart = ARCH->time();
    while (QFileInfo::exists(gateFile)) {
      if (ARCH->time() - gateStart > 900.0) { // 15 min: the launcher is gone — exit rather than hang
        LOG_WARN("MT-gate: never released in 15 min — exiting so the launcher can respawn");
        return s_exitSuccess;
      }
      ARCH->sleep(0.05);
    }
    LOG_INFO("MT-gate: released after %.3fs — starting the KVM engine", ARCH->time() - gateStart);
  }

  EventQueue events;
  const auto processName = QFileInfo(argv[0]).fileName();

  if (parser.serverMode()) {
    ServerApp app(&events, processName);
    return app.run();
  } else if (parser.clientMode()) {
    ClientApp app(&events, processName);
    return app.run();
  }

  return s_exitSuccess;
}
