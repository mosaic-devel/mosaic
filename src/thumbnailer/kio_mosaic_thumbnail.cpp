#include "io/mosaic/preview.hpp"

#include <KIO/ThumbnailCreator>
#include <KPluginFactory>

#include <QImage>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <cstdint>

// The Dolphin half of .mosaic thumbnails (S48-b). KDE's PreviewJob does not read
// share/thumbnailers at all -- it loads KIO ThumbnailCreator plugins from
// qt6/plugins/kf6/thumbcreator, an entirely separate mechanism from the freedesktop entry the
// GNOME-family managers use, with no bridge between them (verified on the dev machine). Same
// contract as mosaic-thumbnailer: newest PRVW only, downscale, hand it over; a file that
// predates previews fails the request and Dolphin shows the mimetype icon.
//
// The build is guarded by find_package(KF6KIO QUIET) -- never a hard dependency.
class MosaicThumbnailCreator : public KIO::ThumbnailCreator {
    Q_OBJECT
public:
    MosaicThumbnailCreator(QObject* parent, const QVariantList& args)
        : KIO::ThumbnailCreator(parent, args) {}

    KIO::ThumbnailResult create(const KIO::ThumbnailRequest& request) override {
        const QString path = request.url().toLocalFile();
        if (path.isEmpty())
            return KIO::ThumbnailResult::fail();
        const auto preview = mosaic::io::native::readNewestPreview(path.toStdString());
        if (!preview.has_value())
            return KIO::ThumbnailResult::fail();
        const int edge =
            std::max({1, request.targetSize().width(), request.targetSize().height()});
        const mosaic::common::Image scaled = mosaic::io::native::downscalePreview(
            *preview, static_cast<std::uint32_t>(edge));
        const QImage img(scaled.rgba.data(), static_cast<int>(scaled.width),
                         static_cast<int>(scaled.height), static_cast<int>(scaled.width * 4),
                         QImage::Format_RGBA8888);
        // copy(): the QImage above only borrows `scaled`, which dies with this frame.
        return KIO::ThumbnailResult::pass(img.copy());
    }
};

K_PLUGIN_CLASS_WITH_JSON(MosaicThumbnailCreator, "mosaicthumbnail.json")

#include "kio_mosaic_thumbnail.moc"
