// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <QKeyEvent>

#include <array>

namespace Noggit
{
  inline constexpr std::array<Qt::Key, 6> physical_movement_keys = {
    Qt::Key_W, Qt::Key_S, Qt::Key_D, Qt::Key_A, Qt::Key_Q, Qt::Key_E
  };

  inline int physicalHotkey(QKeyEvent const& event)
  {
#ifdef Q_OS_WIN
    // Windows reports the scan code independently of the active layout.
    auto const scan_code = event.nativeScanCode();
    if (scan_code >= 0x02 && scan_code <= 0x0b && event.key() != Qt::Key_Plus && event.key() != Qt::Key_Minus)
      return "1234567890"[scan_code - 0x02];
    if (scan_code >= 0x10 && scan_code <= 0x19)
      return "QWERTYUIOP"[scan_code - 0x10];
    if (scan_code >= 0x1e && scan_code <= 0x26)
      return "ASDFGHJKL"[scan_code - 0x1e];
    if (scan_code >= 0x2c && scan_code <= 0x32)
      return "ZXCVBNM"[scan_code - 0x2c];
#else
    Q_UNUSED(event);
#endif
    return 0;
  }
}
