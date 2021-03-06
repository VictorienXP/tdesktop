/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/win/windows_system_media_controls_manager.h"

#include "base/observer.h"
#include "base/platform/win/base_windows_system_media_controls.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "ui/text/format_song_document_name.h"

namespace Platform {

SystemMediaControlsManager::SystemMediaControlsManager(HWND hwnd)
: _controls(std::make_unique<base::Platform::SystemMediaControlsWin>()) {

	using PlaybackStatus =
		base::Platform::SystemMediaControlsWin::PlaybackStatus;
	using Command = base::Platform::SystemMediaControlsWin::Command;

	_controls->init(hwnd);
	const auto type = AudioMsgId::Type::Song;

	const auto mediaPlayer = Media::Player::instance();

	mediaPlayer->updatedNotifier(
	) | rpl::filter([=](const Media::Player::TrackState &state) {
		return state.id.type() == type;
	}) | rpl::map([=](const Media::Player::TrackState &state) {
		using namespace Media::Player;
		if (IsStoppedOrStopping(state.state)) {
			return PlaybackStatus::Stopped;
		} else if (IsPausedOrPausing(state.state)) {
			return PlaybackStatus::Paused;
		}
		return PlaybackStatus::Playing;
	}) | rpl::distinct_until_changed(
	) | rpl::start_with_next([=](PlaybackStatus status) {
		_controls->setPlaybackStatus(status);
	}, _lifetime);

	rpl::merge(
		mediaPlayer->stops(type) | rpl::map_to(false),
		mediaPlayer->startsPlay(type) | rpl::map_to(true)
	) | rpl::start_with_next([=](bool audio) {
		if (audio) {
			_controls->setEnabled(audio);
			_controls->setIsNextEnabled(mediaPlayer->nextAvailable(type));
			_controls->setIsPreviousEnabled(
				mediaPlayer->previousAvailable(type));
			_controls->setIsPlayPauseEnabled(true);
			_controls->setIsStopEnabled(true);
			_controls->setPlaybackStatus(PlaybackStatus::Playing);
			_controls->updateDisplay();
		} else {
			_cachedMediaView.clear();
			_controls->clearMetadata();
		}
		_lifetimeDownload.destroy();
	}, _lifetime);

	auto trackChanged = base::ObservableViewer(
		mediaPlayer->trackChangedNotifier()
	) | rpl::filter([=](AudioMsgId::Type audioType) {
		return audioType == type;
	});

	auto unlocked = Core::App().passcodeLockChanges(
	) | rpl::filter([](bool locked) {
		return !locked;
	}) | rpl::map([=] {
		return type;
	}) | rpl::before_next([=] {
		_controls->setEnabled(true);
		_controls->updateDisplay();
	});

	rpl::merge(
		std::move(trackChanged),
		std::move(unlocked)
	) | rpl::start_with_next([=](AudioMsgId::Type audioType) {
		_lifetimeDownload.destroy();

		const auto current = mediaPlayer->current(audioType);
		if (!current) {
			return;
		}
		const auto document = current.audio();

		const auto &[title, performer] = Ui::Text::FormatSongNameFor(document)
			.composedName();

		_controls->setArtist(performer);
		_controls->setTitle(title);

		if (document && document->isSongWithCover()) {
			const auto view = document->createMediaView();
			view->thumbnailWanted(current.contextId());
			_cachedMediaView.push_back(view);
			if (const auto imagePtr = view->thumbnail()) {
				_controls->setThumbnail(imagePtr->original());
			} else {
				document->session().downloaderTaskFinished(
				) | rpl::start_with_next([=] {
					if (const auto imagePtr = view->thumbnail()) {
						_controls->setThumbnail(imagePtr->original());
						_lifetimeDownload.destroy();
					}
				}, _lifetimeDownload);
				_controls->clearThumbnail();
			}
		} else {
			_controls->clearThumbnail();
		}
	}, _lifetime);

	_controls->commandRequests(
	) | rpl::start_with_next([=](Command command) {
		switch (command) {
		case Command::Play: mediaPlayer->play(type); break;
		case Command::Pause: mediaPlayer->pause(type); break;
		case Command::Next: mediaPlayer->next(type); break;
		case Command::Previous: mediaPlayer->previous(type); break;
		case Command::Stop: mediaPlayer->stop(type); break;
		}
	}, _lifetime);

	Core::App().passcodeLockValue(
	) | rpl::filter([=](bool locked) {
		return locked && Core::App().maybeActiveSession();
	}) | rpl::start_with_next([=] {
		_controls->setEnabled(false);
	}, _lifetime);

}

SystemMediaControlsManager::~SystemMediaControlsManager() = default;

}  // namespace Platform
