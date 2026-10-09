/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "backends/saves/recorder/recorder-saves.h"
#include "gui/EventRecorder.h"
#include "common/savefile.h"

Common::InSaveFile *RecorderSaveFileManager::openForLoading(const Common::String &filename) {
	Common::InSaveFile *result = g_eventRec.processSaveStream(filename);
	return result;
}

Common::StringArray RecorderSaveFileManager::listSavefiles(const Common::String &pattern) {
	return g_eventRec.listSaveFiles(pattern);
}

static bool usesRealSaveManager() {
	GUI::EventRecorder::RecordMode mode = g_eventRec.getRecordMode();
	return mode == GUI::EventRecorder::kRecorderRecord || mode == GUI::EventRecorder::kPassthrough;
}

Common::OutSaveFile *RecorderSaveFileManager::openForSaving(const Common::String &filename, bool compress) {
	// Recording must write through the manager that will subsequently read and
	// list the file. Separate DefaultSaveFileManager instances have separate caches.
	if (usesRealSaveManager())
		return g_eventRec.getRealSaveManager()->openForSaving(filename, compress);
	return DefaultSaveFileManager::openForSaving(filename, compress);
}

Common::SeekableReadStream *RecorderSaveFileManager::openRawFile(const Common::String &filename) {
	if (usesRealSaveManager())
		return g_eventRec.getRealSaveManager()->openRawFile(filename);
	return DefaultSaveFileManager::openRawFile(filename);
}

bool RecorderSaveFileManager::removeSavefile(const Common::String &filename) {
	if (usesRealSaveManager())
		return g_eventRec.getRealSaveManager()->removeSavefile(filename);
	return DefaultSaveFileManager::removeSavefile(filename);
}

bool RecorderSaveFileManager::exists(const Common::String &filename) {
	if (usesRealSaveManager())
		return g_eventRec.getRealSaveManager()->exists(filename);
	GUI::EventRecorder::RecordMode mode = g_eventRec.getRecordMode();
	if (mode == GUI::EventRecorder::kRecorderPlayback || mode == GUI::EventRecorder::kRecorderUpdate) {
		for (const Common::String &name : listSavefiles("*")) {
			if (name.equalsIgnoreCase(filename))
				return true;
		}
		return false;
	}
	return DefaultSaveFileManager::exists(filename);
}

void RecorderSaveFileManager::updateSavefilesList(Common::StringArray &lockedFiles) {
	if (usesRealSaveManager())
		g_eventRec.getRealSaveManager()->updateSavefilesList(lockedFiles);
	else
		DefaultSaveFileManager::updateSavefilesList(lockedFiles);
}

void RecorderSaveFileManager::clearError() {
	if (usesRealSaveManager())
		g_eventRec.getRealSaveManager()->clearError();
	else
		DefaultSaveFileManager::clearError();
}

Common::Error RecorderSaveFileManager::getError() {
	return usesRealSaveManager() ? g_eventRec.getRealSaveManager()->getError() : DefaultSaveFileManager::getError();
}

Common::String RecorderSaveFileManager::getErrorDesc() {
	return usesRealSaveManager() ? g_eventRec.getRealSaveManager()->getErrorDesc() : DefaultSaveFileManager::getErrorDesc();
}

Common::String RecorderSaveFileManager::popErrorDesc() {
	return usesRealSaveManager() ? g_eventRec.getRealSaveManager()->popErrorDesc() : DefaultSaveFileManager::popErrorDesc();
}
