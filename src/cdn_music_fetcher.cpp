#include "cdn_music_fetcher.h"

#include "config_manager.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>

const QString CdnMusicFetcher::MUSIC_DIRECTORY = QStringLiteral("sounds/music/");
const QString CdnMusicFetcher::CACHE_PATH = QStringLiteral("config/music_cdn_cache.json");

CdnMusicFetcher::CdnMusicFetcher(QObject *parent) :
    QObject(parent),
    m_network(new QNetworkAccessManager(this)),
    m_refresh_timer(new QTimer(this))
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    m_network->setTransferTimeout(std::chrono::milliseconds(TIMEOUT_MS));
#else
    m_network->setTransferTimeout(TIMEOUT_MS);
#endif
    m_refresh_timer->setSingleShot(false);
    connect(m_refresh_timer, &QTimer::timeout, this, [this] { start(true); });
}

bool CdnMusicFetcher::isRunning() const
{
    return m_running;
}

QString CdnMusicFetcher::normaliseBase(const QString &f_cdn)
{
    QString l_cdn = f_cdn.trimmed();
    if (l_cdn.isEmpty() || l_cdn.startsWith('#')) {
        return QString();
    }
    if (!l_cdn.contains("://")) {
        l_cdn = "https://" + l_cdn;
    }
    if (!l_cdn.endsWith('/')) {
        l_cdn += '/';
    }
    const QUrl l_url(l_cdn);
    if (!l_url.isValid() || l_url.host().isEmpty() || (l_url.scheme() != "https" && l_url.scheme() != "http")) {
        return QString();
    }
    return l_cdn;
}

bool CdnMusicFetcher::isAudioFile(const QString &f_path)
{
    static const QStringList l_extensions = {".opus", ".ogg", ".mp3", ".wav"};
    for (const QString &l_ext : l_extensions) {
        if (f_path.endsWith(l_ext, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QStringList CdnMusicFetcher::parseLinks(const QByteArray &f_html)
{
    static const QRegularExpression l_href(QStringLiteral("href\\s*=\\s*[\"']([^\"']+)[\"']"), QRegularExpression::CaseInsensitiveOption);
    QStringList l_links;
    const QString l_text = QString::fromUtf8(f_html);
    QRegularExpressionMatchIterator l_it = l_href.globalMatch(l_text);
    while (l_it.hasNext()) {
        l_links.append(l_it.next().captured(1));
    }
    return l_links;
}

bool CdnMusicFetcher::cacheIsFresh() const
{
    QFile l_file(CACHE_PATH);
    if (!l_file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject l_root = QJsonDocument::fromJson(l_file.readAll()).object();
    const QDateTime l_fetched = QDateTime::fromString(l_root.value("fetched").toString(), Qt::ISODate);
    if (!l_fetched.isValid()) {
        return false;
    }

    // A changed CDN list invalidates the cache.
    QStringList l_cached_cdns;
    const QJsonArray l_categories = l_root.value("music").toArray();
    for (const QJsonValue &l_category : l_categories) {
        l_cached_cdns.append(l_category.toObject().value("cdn").toString());
    }
    for (const QString &l_cdn : ConfigManager::musicCdnList()) {
        const QString l_base = normaliseBase(l_cdn);
        if (!l_base.isEmpty() && !l_cached_cdns.contains(l_base)) {
            return false;
        }
    }

    return l_fetched.secsTo(QDateTime::currentDateTimeUtc()) < qint64(ConfigManager::cdnMusicRefreshHours()) * 3600;
}

void CdnMusicFetcher::start(bool f_force)
{
    if (ConfigManager::musicCdnList().isEmpty()) {
        m_refresh_timer->stop();
        return;
    }

    const int l_hours = ConfigManager::cdnMusicRefreshHours();
    if (l_hours > 0) {
        m_refresh_timer->start(l_hours * 3600 * 1000);
    }
    else {
        m_refresh_timer->stop();
    }

    if (m_running) {
        return;
    }
    if (!f_force && cacheIsFresh()) {
        return;
    }
    beginScan();
}

void CdnMusicFetcher::beginScan()
{
    m_running = true;
    m_results.clear();
    m_visited.clear();
    m_pending = 0;

    const QStringList l_cdns = ConfigManager::musicCdnList();
    for (const QString &l_cdn : l_cdns) {
        const QString l_base = normaliseBase(l_cdn);
        if (l_base.isEmpty()) {
            continue;
        }
        CdnResult l_result;
        l_result.base = l_base;
        l_result.root = l_base + MUSIC_DIRECTORY;
        m_results.append(l_result);
    }

    if (m_results.isEmpty()) {
        m_running = false;
        return;
    }

    qInfo() << "Scanning" << m_results.size() << "CDN(s) for music...";
    m_requests_made = 0;
    for (int i = 0; i < m_results.size(); i++) {
        crawl(i, QUrl(m_results.at(i).root), 0);
    }
}

void CdnMusicFetcher::crawl(int f_cdn_index, const QUrl &f_url, int f_depth)
{
    if (f_depth > MAX_DEPTH || m_requests_made >= MAX_REQUESTS_PER_CDN * m_results.size()) {
        return;
    }
    if (m_visited.contains(f_url.toString())) {
        return;
    }
    m_visited.insert(f_url.toString());
    m_requests_made++;

    QNetworkRequest l_request(f_url);
    l_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    l_request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("akashi-cdn-music-fetcher"));

    QNetworkReply *l_reply = m_network->get(l_request);
    l_reply->setProperty("cdn_index", f_cdn_index);
    l_reply->setProperty("depth", f_depth);
    m_pending++;
    connect(l_reply, &QNetworkReply::finished, this, &CdnMusicFetcher::requestFinished);
}

void CdnMusicFetcher::requestFinished()
{
    QNetworkReply *l_reply = qobject_cast<QNetworkReply *>(sender());
    if (!l_reply) {
        return;
    }
    l_reply->deleteLater();
    m_pending--;

    const int l_index = l_reply->property("cdn_index").toInt();
    const int l_depth = l_reply->property("depth").toInt();
    CdnResult &l_result = m_results[l_index];
    const QUrl l_url = l_reply->url();

    if (l_reply->error() == QNetworkReply::NoError) {
        // The listing of the music directory itself decides whether a CDN counts as reachable.
        // If it redirected, everything below is matched against the final location.
        if (l_depth == 0) {
            l_result.ok = true;
            l_result.root = l_url.toString();
            if (!l_result.root.endsWith('/')) {
                l_result.root += '/';
            }
        }
        const QUrl l_root(l_result.root);

        const QStringList l_links = parseLinks(l_reply->readAll());
        for (const QString &l_link : l_links) {
            if (l_link.startsWith('?') || l_link.startsWith('#')) {
                continue; // Sorting links of autoindex pages.
            }
            const QUrl l_target = l_url.resolved(QUrl(l_link));

            // Only follow links that stay below the music directory. This also drops "Parent Directory".
            if (l_target.host() != l_root.host() || !l_target.path().startsWith(l_root.path()) || l_target.path() == l_root.path() || l_target.path() == l_url.path()) {
                continue;
            }
            if (!l_target.query().isEmpty() || !l_target.fragment().isEmpty()) {
                continue;
            }

            if (l_target.path().endsWith('/')) {
                crawl(l_index, l_target, l_depth + 1);
            }
            else {
                const QString l_relative = l_target.path().mid(l_root.path().length());
                if (isAudioFile(l_relative) && !l_result.files.contains(l_relative)) {
                    l_result.files.append(l_relative);
                }
            }
        }
    }
    else {
        qWarning() << "CDN music scan failed for" << l_url.toString() << ":" << l_reply->errorString();
    }

    if (m_pending == 0) {
        finishScan();
    }
}

void CdnMusicFetcher::finishScan()
{
    m_running = false;

    // Entries of CDNs that could not be reached are carried over from the old cache.
    QJsonArray l_old_categories;
    {
        QFile l_old(CACHE_PATH);
        if (l_old.open(QIODevice::ReadOnly)) {
            l_old_categories = QJsonDocument::fromJson(l_old.readAll()).object().value("music").toArray();
        }
    }

    QJsonArray l_music;
    bool l_any_ok = false;
    for (const CdnResult &l_result : qAsConst(m_results)) {
        if (!l_result.ok) {
            for (const QJsonValue &l_old_category : qAsConst(l_old_categories)) {
                if (l_old_category.toObject().value("cdn").toString() == l_result.base) {
                    l_music.append(l_old_category);
                }
            }
            continue;
        }
        l_any_ok = true;

        QStringList l_files = l_result.files;
        l_files.sort(Qt::CaseInsensitive);

        QJsonArray l_songs;
        for (const QString &l_file : qAsConst(l_files)) {
            QJsonObject l_song;
            l_song["name"] = l_file;
            l_song["length"] = -1;
            l_song["cdn"] = l_result.base;
            l_songs.append(l_song);
        }

        QJsonObject l_category;
        l_category["category"] = QString("== CDN: %1 ==").arg(QUrl(l_result.base).host());
        l_category["cdn"] = l_result.base;
        l_category["songs"] = l_songs;
        l_music.append(l_category);
        qInfo() << "Found" << l_files.size() << "song(s) on" << l_result.base;
    }

    if (!l_any_ok) {
        qWarning() << "No CDN could be scanned. Keeping the existing music cache.";
        return;
    }

    QJsonObject l_root;
    l_root["fetched"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    l_root["music"] = l_music;

    QSaveFile l_cache(CACHE_PATH);
    if (!l_cache.open(QIODevice::WriteOnly) || l_cache.write(QJsonDocument(l_root).toJson(QJsonDocument::Indented)) < 0 || !l_cache.commit()) {
        qWarning() << "Unable to write" << CACHE_PATH;
        return;
    }

    emit musicListUpdated();
}
