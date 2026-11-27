/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include "../export.h"

#include <atomic>
#include <thread>

// out.xmp + out.smp for nugget's xmplayer (format version 2) from a PS1 SPU song.
class DivExportXMP: public DivROMExport {
  DivEngine* e;
  std::thread* exportThread;
  DivROMExportProgress progress[2];
  std::atomic<bool> running, failed, mustAbort;
  // written by the export thread, read by getProgress
  std::atomic<float> progressAmount;
  void run();
  public:
    bool go(DivEngine* e);
    bool isRunning();
    bool hasFailed();
    void abort();
    void wait();
    DivROMExportProgress getProgress(int index=0);
    DivExportXMP():
      e(NULL),
      exportThread(NULL),
      running(false),
      failed(false),
      mustAbort(false) {}
    ~DivExportXMP() {}
};
