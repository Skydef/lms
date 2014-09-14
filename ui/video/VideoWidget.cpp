#include <Wt/WApplication>
#include <Wt/WEnvironment>

#include "logger/Logger.hpp"

#include "VideoWidget.hpp"

namespace UserInterface {

VideoWidget::VideoWidget(SessionData& sessionData, Wt::WContainerWidget* parent )
: Wt::WContainerWidget(parent),
_sessionData(sessionData)
{

	_videoDbWidget = new VideoDatabaseWidget(_sessionData.getDatabaseHandler(), this);

	_videoDbWidget->playVideo().connect(this, &VideoWidget::playVideo);


}


void
VideoWidget::search(const std::string& searchText)
{
	//TODO
}

void
VideoWidget::backToList(void)
{
	if (_mediaPlayer)
		delete _mediaPlayer;

	_videoDbWidget->setHidden(false);

}


void
VideoWidget::playVideo(boost::filesystem::path p)
{
	LMS_LOG(MOD_UI, SEV_DEBUG) << "Want to play video " << p << "'" << std::endl;
	try {

		std::size_t audioBitrate = 0;
		std::size_t videoBitrate = 0;

		// Get user preferences
		{
			Wt::Dbo::Transaction transaction(_sessionData.getDatabaseHandler().getSession());
			Database::User::pointer user = _sessionData.getDatabaseHandler().getCurrentUser();
			if (user)
			{
				audioBitrate = user->getMaxAudioBitrate();
				videoBitrate = user->getMaxVideoBitrate();
			}
			else
			{
				LMS_LOG(MOD_UI, SEV_ERROR) << "Can't play video: user does not exists!";
				return; // TODO logout?
			}
		}

		LMS_LOG(MOD_UI, SEV_DEBUG) << "Max bitrate set to " << videoBitrate << "/" << audioBitrate;

		Transcode::InputMediaFile inputFile(p);

		Transcode::Format::Encoding encoding;

		if (Wt::WApplication::instance()->environment().agentIsChrome())
			encoding = Transcode::Format::WEBMV;
		else
			encoding = Transcode::Format::FLV;

		Transcode::Parameters parameters(inputFile, Transcode::Format::get(encoding));

		// TODO, make a quality button in order to choose...

		parameters.setBitrate(Transcode::Stream::Audio, 0/*audioBitrate*/);
		parameters.setBitrate(Transcode::Stream::Video, 0/*videoBitrate*/);

		_mediaPlayer = new VideoMediaPlayerWidget(parameters, this);
		_mediaPlayer->close().connect(this, &VideoWidget::backToList);

		_videoDbWidget->setHidden(true);
	}
	catch( std::exception& e) {
		LMS_LOG(MOD_UI, SEV_ERROR) << "Caught exception while loading '" << p << "': " << e.what() << std::endl;
	}
}

} // namespace UserInterface

