/**********************************************************************
 *  Lightweight GitHub-API update checker.
 *
 *  Pings api.github.com/repos/zvldz/Win32DiskImager/releases/latest,
 *  parses the tag, compares against the running APP_VERSION and emits
 *  the appropriate signal so the MainWindow can show a dialog.
 *
 *  - check(forced=false) is the silent path used by the weekly
 *    auto-check at startup. On success-with-no-update or any error it
 *    stays silent.
 *  - check(forced=true) is the user-initiated path (clicking the
 *    version link / system-menu entry). Emits noUpdateAvailable()
 *    or checkFailed() so the user gets feedback either way.
 **********************************************************************/

#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QNetworkReply;

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);

    void check(bool forced);

signals:
    // tag has the leading "v" stripped (e.g. "2.2.3").
    // installerUrl is empty when no Win32DiskImager-setup-<tag>.exe asset
    // is attached to the release.
    void updateAvailable(const QString &tag, const QString &installerUrl);
    // Forced only. Auto-check stays silent in this case.
    void noUpdateAvailable();
    void checkFailed(const QString &error);

private slots:
    void onReplyFinished();

private:
    QNetworkAccessManager m_nam;
    bool m_forced = false;
};

// "2.2.10" > "2.2.9" — splits on dots and compares numerically. Returns
// negative / zero / positive like memcmp.
int compareVersions(const QString &a, const QString &b);

#endif // UPDATECHECKER_H
