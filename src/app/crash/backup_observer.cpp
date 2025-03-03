// Aseprite
// Copyright (C) 2018-2019  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

// Uncomment if you want to test the backup process each 5 seconds.
//#define TEST_BACKUPS_WITH_A_SHORT_PERIOD

// Uncomment if you want to check that backups are correctly saved
// after being saved.
//#define TEST_BACKUP_INTEGRITY

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/crash/backup_observer.h"

#include "app/app.h"
#include "app/context.h"
#include "app/crash/recovery_config.h"
#include "app/crash/session.h"
#include "app/doc.h"
#include "app/doc_access.h"
#include "app/doc_diff.h"
#include "app/pref/preferences.h"
#include "base/bind.h"
#include "base/chrono.h"
#include "base/remove_from_container.h"
#include "ui/system.h"

namespace app {
namespace crash {

namespace {

class SwitchBackupIcon {
public:
  SwitchBackupIcon() {
    ui::execute_from_ui_thread(
      []{
        if (App* app = App::instance())
          app->showBackupNotification(true);
      });
  }
  ~SwitchBackupIcon() {
    ui::execute_from_ui_thread(
      []{
        if (App* app = App::instance())
          app->showBackupNotification(false);
      });
  }
};

}

BackupObserver::BackupObserver(RecoveryConfig* config,
                               Session* session,
                               Context* ctx)
  : m_config(config)
  , m_session(session)
  , m_ctx(ctx)
  , m_done(false)
  , m_thread(base::Bind<void>(&BackupObserver::backgroundThread, this))
{
  m_ctx->add_observer(this);
  m_ctx->documents().add_observer(this);
}

BackupObserver::~BackupObserver()
{
  m_thread.join();
  m_ctx->documents().remove_observer(this);
  m_ctx->remove_observer(this);
}

void BackupObserver::stop()
{
  m_done = true;
  m_wakeup.notify_one();
}

void BackupObserver::onAddDocument(Doc* document)
{
  TRACE("RECO: Observe document %p\n", document);

  std::unique_lock<std::mutex> lock(m_mutex);
  m_documents.push_back(document);
}

void BackupObserver::onRemoveDocument(Doc* doc)
{
  TRACE("RECO: Remove document %p\n", doc);
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    base::remove_from_container(m_documents, doc);
  }
  if (m_config->keepEditedSpriteData)
    m_closedDocs.push_back(doc);
  else
    m_session->removeDocument(doc);
}

void BackupObserver::backgroundThread()
{
  std::unique_lock<std::mutex> lock(m_mutex);

  int normalPeriod = int(60.0*m_config->dataRecoveryPeriod);
  int lockedPeriod = 5;
#ifdef TEST_BACKUPS_WITH_A_SHORT_PERIOD
  normalPeriod = 5;
  lockedPeriod = 5;
#endif

  int waitFor = normalPeriod;

  while (!m_done) {
    m_wakeup.wait_for(lock, std::chrono::seconds(waitFor));

    TRACE("RECO: Start backup process for %d documents\n", m_documents.size());

    SwitchBackupIcon icon;
    base::Chrono chrono;
    bool somethingLocked = false;

    for (Doc* doc : m_documents) {
      if (!saveDocData(doc))
        somethingLocked = true;
    }

    if (!m_closedDocs.empty()) {
      for (auto it=m_closedDocs.begin(); it != m_closedDocs.end(); ) {
        Doc* doc = *it;
        if (saveDocData(doc))
          it = m_closedDocs.erase(it);
        else {
          somethingLocked = true;
          ++it;
        }
      }
    }

    waitFor = (somethingLocked ? lockedPeriod: normalPeriod);

    TRACE("RECO: Backup process done (%.16g)\n", chrono.elapsed());
  }
}

// Executed from the backgroundThread() (non-UI thread)
bool BackupObserver::saveDocData(Doc* doc)
{
  try {
    if (!doc->needsBackup())
      return true;

    if (doc->inhibitBackup()) {
      TRACE("RECO: Document '%d' backup is temporarily inhibited\n", doc->id());
    }
    else if (!m_session->saveDocumentChanges(doc)) {
      TRACE("RECO: Document '%d' backup was canceled by UI\n", doc->id());
    }
#ifdef TEST_BACKUP_INTEGRITY
    else {
      DocReader reader(doc, 500);
      std::unique_ptr<Doc> copy(
        m_session->restoreBackupDocById(doc->id(), nullptr));
      DocDiff diff = compare_docs(doc, copy.get());
      if (diff.anything) {
        TRACE("RECO: Differences (%s/%s/%s/%s/%s/%s/%s)\n",
              diff.canvas ? "canvas": "",
              diff.totalFrames ? "totalFrames": "",
              diff.frameDuration ? "frameDuration": "",
              diff.frameTags ? "frameTags": "",
              diff.palettes ? "palettes": "",
              diff.layers ? "layers": "",
              diff.cels ? "cels": "",
              diff.images ? "images": "",
              diff.colorProfiles ? "colorProfiles": "");

        Doc* copyDoc = copy.release();
        ui::execute_from_ui_thread(
          [this, copyDoc] {
            m_ctx->documents().add(copyDoc);
          });
      }
      else {
        TRACE("RECO: No differences\n");
      }
      return true;
    }
#endif
  }
  catch (const std::exception&) {
    TRACE("RECO: Document '%d' is locked\n", doc->id());
  }
  return false;
}

} // namespace crash
} // namespace app
