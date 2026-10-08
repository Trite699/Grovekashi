#ifndef CDN_MUSIC_FETCHER_H
#define CDN_MUSIC_FETCHER_H

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>

/**
 * @brief Scans remote Attorney Online content servers (CDNs) for music and caches the result.
 *
 * For every CDN base URL listed in config/text/music_cdns.txt, the `sounds/music/` directory is
 * crawled through its HTML directory index. Every audio file found becomes a musiclist entry:
 *
 *   {"name": "[AJ] Epilogue.opus", "length": -1, "cdn": "https://attorneyoffline.de/newvanillabase/"}
 *
 * The entries are stored in config/music_cdn_cache.json, which uses the same layout as music.json,
 * and are merged into the root musiclist by ConfigManager::musiclist().
 */
class CdnMusicFetcher : public QObject
{
    Q_OBJECT

  public:
    /// Directory on an AO content server that holds all music.
    static const QString MUSIC_DIRECTORY;

    /// Location of the cache file, relative to the server working directory.
    static const QString CACHE_PATH;

    explicit CdnMusicFetcher(QObject *parent = nullptr);

    /**
     * @brief Starts a scan if the cache is older than the refresh interval, or if @p f_force is set.
     *
     * Also (re)arms the periodic refresh timer.
     */
    void start(bool f_force = false);

    /**
     * @brief Returns true while a scan is running.
     */
    bool isRunning() const;

  signals:
    /**
     * @brief Emitted after the cache file has been rewritten with new data.
     */
    void musicListUpdated();

  private:
    struct CdnResult
    {
        QString base;
        QString root; //!< URL of the music directory. Replaced by the final URL if the CDN redirects.
        bool ok = false;
        QStringList files; //!< Paths relative to the music directory, decoded.
    };

    void beginScan();
    void crawl(int f_cdn_index, const QUrl &f_url, int f_depth);
    void requestFinished();
    void finishScan();
    bool cacheIsFresh() const;
    static QString normaliseBase(const QString &f_cdn);
    static QStringList parseLinks(const QByteArray &f_html);
    static bool isAudioFile(const QString &f_path);

    QNetworkAccessManager *m_network;
    QTimer *m_refresh_timer;
    QList<CdnResult> m_results;
    QSet<QString> m_visited;
    int m_pending = 0;
    int m_requests_made = 0;
    bool m_running = false;

    static constexpr int MAX_DEPTH = 8;
    static constexpr int MAX_REQUESTS_PER_CDN = 2000;
    static constexpr int TIMEOUT_MS = 15000;
};

#endif // CDN_MUSIC_FETCHER_H
