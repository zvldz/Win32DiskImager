/**********************************************************************
 *  UpdateChecker implementation — see updatechecker.h.
 **********************************************************************/

#include "updatechecker.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>

#include "version.h"

static const char *kReleasesUrl =
    "https://api.github.com/repos/zvldz/Win32DiskImager/releases/latest";

int compareVersions(const QString &a, const QString &b)
{
    const QStringList ap = a.split('.');
    const QStringList bp = b.split('.');
    const int n = std::max(ap.size(), bp.size());
    for (int i = 0; i < n; ++i) {
        const int av = (i < ap.size()) ? ap.at(i).toInt() : 0;
        const int bv = (i < bp.size()) ? bp.at(i).toInt() : 0;
        if (av != bv) return av - bv;
    }
    return 0;
}

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent) {}

void UpdateChecker::check(bool forced)
{
    m_forced = forced;
    QNetworkRequest req((QUrl(kReleasesUrl)));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QByteArray("Win32DiskImager-UpdateChecker"));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, &UpdateChecker::onReplyFinished);
}

void UpdateChecker::onReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        if (m_forced) emit checkFailed(reply->errorString());
        return;
    }

    const QByteArray body = reply->readAll();
    QJsonParseError parseErr {};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (m_forced) emit checkFailed(tr("Malformed response from GitHub"));
        return;
    }
    const QJsonObject root = doc.object();
    QString tag = root.value("tag_name").toString();
    if (tag.isEmpty()) {
        if (m_forced) emit checkFailed(tr("Release has no tag_name"));
        return;
    }
    if (tag.startsWith('v') || tag.startsWith('V')) {
        tag.remove(0, 1);
    }

    if (compareVersions(tag, QStringLiteral(APP_VERSION)) <= 0) {
        if (m_forced) emit noUpdateAvailable();
        return;
    }

    // Find the Win32DiskImager-setup-<tag>.exe asset, if present.
    QString installerUrl;
    const QString want = QStringLiteral("Win32DiskImager-setup-%1.exe").arg(tag);
    const QJsonArray assets = root.value("assets").toArray();
    for (const QJsonValue &v : assets) {
        const QJsonObject o = v.toObject();
        if (o.value("name").toString().compare(want, Qt::CaseInsensitive) == 0) {
            installerUrl = o.value("browser_download_url").toString();
            break;
        }
    }

    emit updateAvailable(tag, installerUrl);
}
