/*
 *  Copyright (C) 2007  Thiago Macieira <thiago@kde.org>
 *  Copyright (C) 2009 Thomas Zander <zander@kde.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "repository.h"
#include "options.hpp"
#include "log.hpp"
#include <QTextStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLinkedList>
#include <boost/foreach.hpp>
#include <sstream>
#include <stdexcept>

#include "svn_revision.hpp"

static const int maxSimultaneousProcesses = 100;

static const int maxMark = (1 << 20) - 2; // some versions of git-fast-import are buggy for larger values of maxMark

class ProcessCache: QLinkedList<Repository *>
  {
  public:
    void touch(Repository *repo)
      {
      remove(repo);

      // if the cache is too big, remove from the front
      while (size() >= maxSimultaneousProcesses)
          takeFirst()->closeFastImport();

      // append to the end
      append(repo);
      }

    inline void remove(Repository *repo)
      {
#if QT_VERSION >= 0x040400
      removeOne(repo);
#else
      removeAll(repo);
#endif
      }
  };
static ProcessCache processCache;

static QString marksFileName(QString name)
  {
  name.replace('/', '_');
  name.prepend("marks-");
  return name;
  }

Repository::Repository(
    const Ruleset::Repository &rule,
    bool incremental,
    QHash<QString, Repository*> const& repo_index)
    : name(QString::fromStdString(rule.name))
    , prefix(/*rule.forwardTo*/)
    , submodule_in_repo(
        rule.submodule_in_repo.empty()
        ? 0 : repo_index[QString::fromStdString(rule.submodule_in_repo)] )
    , submodule_path( QString::fromStdString(rule.submodule_path) )
    , fastImport(name)
    , commitCount(0)
    , outstandingTransactions(0)
    , last_commit_mark(0)
    , next_file_mark(maxMark)
    , processHasStarted(false)
    , incremental(incremental)
  {
  BOOST_FOREACH(boost2git::BranchRule const* branch, rule.branches)
    {
    branches[QString::fromStdString(qualify_ref(branch->name, branch->git_ref_qualifier))].created = 0;
    }

  // create the default branch
  branches[QString("refs/heads/master")].created = 1;

  fastImport.setWorkingDirectory(name);
  if (!options.dry_run) {
    if (!QDir(name).exists()) { // repo doesn't exist yet.
      Log::trace() << "Creating new repository " << qPrintable(name) << std::endl;
      QDir::current().mkpath(name);
      QProcess init;
      init.setWorkingDirectory(name);
      init.start("git", QStringList() << "--bare" << "init");
      init.waitForFinished(-1);
//            // Write description
//            if (!rule.description.isEmpty()) {
//                QFile fDesc(QDir(name).filePath("description"));
//                if (fDesc.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
//                    fDesc.write(rule.description.toUtf8());
//                    fDesc.putChar('\n');
//                    fDesc.close();
//                }
//            }
      {
      QFile marks(name + "/" + marksFileName(name));
      marks.open(QIODevice::WriteOnly);
      marks.close();
      }
    }
  }
  }

static QString logFileName(QString name)
  {
  name.replace('/', '_');
  name.prepend("log-");
  return name;
  }

static int lastValidMark(QString name)
  {
  QFile marksfile(name + "/" + marksFileName(name));
  if (!marksfile.open(QIODevice::ReadOnly))
      return 0;

  int prev_mark = 0;

  int lineno = 0;
  while (!marksfile.atEnd()) {
    QString line = marksfile.readLine();
    ++lineno;
    if (line.isEmpty())
        continue;

    int mark = 0;
    if (line[0] == ':') {
      int sp = line.indexOf(' ');
      if (sp != -1) {
        QString m = line.mid(1, sp-1);
        mark = m.toInt();
      }
    }

    if (!mark) {
      qCritical() << marksfile.fileName() << "line" << lineno << "marks file corrupt?";
      return 0;
    }

    if (mark == prev_mark) {
      qCritical() << marksfile.fileName() << "line" << lineno << "marks file has duplicates";
      return 0;
    }

    if (mark < prev_mark) {
      qCritical() << marksfile.fileName() << "line" << lineno << "marks file not sorted";
      return 0;
    }

    if (mark > prev_mark + 1)
        break;

    prev_mark = mark;
  }

  return prev_mark;
  }

int Repository::setupIncremental(int &cutoff)
  {
  QFile logfile(logFileName(name));
  if (!logfile.exists())
      return 1;

  logfile.open(QIODevice::ReadWrite);

  QRegExp progress("progress SVN r(\\d+) branch (.*) = :(\\d+)");

  int last_valid_mark = lastValidMark(name);

  int last_revnum = 0;
  qint64 pos = 0;
  int retval = 0;
  QString bkup = logfile.fileName() + ".old";

  while (!logfile.atEnd()) {
    pos = logfile.pos();
    QByteArray line = logfile.readLine();
    int hash = line.indexOf('#');
    if (hash != -1)
        line.truncate(hash);
    line = line.trimmed();
    if (line.isEmpty())
        continue;
    if (!progress.exactMatch(line))
        continue;

    int revnum = progress.cap(1).toInt();
    QString branch = progress.cap(2);
    int mark = progress.cap(3).toInt();

    if (revnum >= cutoff)
        goto beyond_cutoff;

    if (revnum < last_revnum)
      {
      Log::warn() << qPrintable(name) << " revision numbers are not monotonic: "
                  << " got " << last_revnum << " and then " << revnum << std::endl;
      }

    if (mark > last_valid_mark)
      {
      Log::warn() << qPrintable(name) << " unknown commit mark found:"
                  << " rewinding -- did you hit Ctrl-C?" << std::endl;
      cutoff = revnum;
      goto beyond_cutoff;
      }

    last_revnum = revnum;

    if (last_commit_mark < mark)
        last_commit_mark = mark;

    Branch &br = branches[branch];
    if (!br.created || !mark || br.marks.isEmpty() || !br.marks.last())
        br.created = revnum;
    br.commits.append(revnum);
    br.marks.append(mark);
  }

  retval = last_revnum + 1;
  if (retval == cutoff)
      /*
       * If a stale backup file exists already, remove it, so that
       * we don't confuse ourselves in 'restoreLog()'
       */
      QFile::remove(bkup);

  return retval;

beyond_cutoff:
  // backup file, since we'll truncate
  QFile::remove(bkup);
  logfile.copy(bkup);

  // truncate, so that we ignore the rest of the revisions
  Log::debug() << qPrintable(name) << " truncating history to revision "
               << cutoff << std::endl;
  logfile.resize(pos);
  return cutoff;
  }

void Repository::restoreLog()
  {
  QString file = logFileName(name);
  QString bkup = file + ".old";
  if (!QFile::exists(bkup))
      return;
  QFile::remove(file);
  QFile::rename(bkup, file);
  }

Repository::~Repository()
  {
  Q_ASSERT(outstandingTransactions == 0);
  closeFastImport();
  }

void Repository::closeFastImport()
  {
  if (fastImport.state() != QProcess::NotRunning)
    {
    fastImport.write("checkpoint\n");
    fastImport.waitForBytesWritten(-1);
    fastImport.closeWriteChannel();
    if (!fastImport.waitForFinished())
      {
      fastImport.terminate();
      if (!fastImport.waitForFinished(200))
        {
        Log::warn() << "git-fast-import for repository "
                    << qPrintable(name) << " did not die" << std::endl;
        }
      }
    }
  processHasStarted = false;
  processCache.remove(this);
  }

void Repository::reloadBranches()
  {
  bool reset_notes = false;
  foreach (QString branch, branches.keys()) {
    Q_ASSERT(branch.startsWith("refs/"));
    Branch &br = branches[branch];

    if (br.marks.isEmpty() || !br.marks.last())
        continue;

    reset_notes = true;

    QByteArray branchRef = branch.toUtf8();

    fastImport.write("reset " + branchRef +
      "\nfrom :" + QByteArray::number(br.marks.last()) + "\n\n"
      "progress Branch " + branchRef + " reloaded\n");
  }

  if (reset_notes && options.add_metadata_notes) {
    fastImport.write("reset refs/notes/commits\nfrom :" +
      QByteArray::number(maxMark + 1) +
      "\n");
  }
  }

int Repository::markFrom(const QString &branchFrom, int branchRevNum, QByteArray &branchFromDesc)
  {
  Q_ASSERT(branchFrom.startsWith("refs/"));

  Branch &brFrom = branches[branchFrom];
  if (!brFrom.created)
      return -1;

  if (brFrom.commits.isEmpty()) {
    return -1;
  }
  if (branchRevNum == brFrom.commits.last()) {
    return brFrom.marks.last();
  }

  QVector<int>::const_iterator it = qUpperBound(brFrom.commits, branchRevNum);
  if (it == brFrom.commits.begin()) {
    return 0;
  }

  int closestCommit = *--it;

  if (!branchFromDesc.isEmpty()) {
    branchFromDesc += " at r" + QByteArray::number(branchRevNum);
    if (closestCommit != branchRevNum) {
      branchFromDesc += " => r" + QByteArray::number(closestCommit);
    }
  }

  return brFrom.marks[it - brFrom.commits.begin()];
  }

int Repository::createBranch(
    BranchRule const* branch_rule,
    SvnRevision* svn_revision,
    const QString &branchFrom,
    int branchRevNum)
  {
  QString branch = QString::fromStdString(git_ref_name(branch_rule));
  
  Q_ASSERT(branch.startsWith("refs/"));
  Q_ASSERT(branchFrom.startsWith("refs/"));
  QByteArray branchFromDesc = "from branch " + branchFrom.toUtf8();
  int mark = markFrom(branchFrom, branchRevNum, branchFromDesc);

  if (mark == -1)
    {
    std::stringstream message;
    message << qPrintable(branch) << " in repository " << qPrintable(name)
            << " is branching from branch " << qPrintable(branchFrom)
            << " but the latter doesn't exist. Can't continue.";
    throw std::runtime_error(message.str());
    }
  QByteArray branchFromRef = ":" + QByteArray::number(mark);
  if (!mark)
    {
    Log::warn() << qPrintable(branch) << " in repository "
                << qPrintable(name) << " is branching but no exported commits exist in repository."
                << " creating an empty branch." << std::endl;
    branchFromRef = branchFrom.toUtf8();
    branchFromDesc += ", deleted/unknown";
    }
  Log::debug() << "Creating branch: " << qPrintable(branch) << " from "
               << qPrintable(branchFrom) << " (r" << branchRevNum << ' '
               << qPrintable(branchFromDesc) << ')' << " in repository "
               << qPrintable(name) << std::endl;
  // Preserve note
  branches[branch].note = branches.value(branchFrom).note;
  return resetBranch(branch_rule, branch, svn_revision, mark, branchFromRef, branchFromDesc);
  }

int Repository::deleteBranch(BranchRule const* branch_rule, SvnRevision* svn_revision)
  {
  QString branch = QString::fromStdString(git_ref_name(branch_rule));
  Q_ASSERT(branch.startsWith("refs/"));

  if (branch == "refs/heads/master")
      return EXIT_SUCCESS;

  static QByteArray null_sha(40, '0');
  return resetBranch(branch_rule, branch, svn_revision, 0, null_sha, "delete");
  }

int Repository::resetBranch(
    BranchRule const* branch_rule,
    const QString &branch,  // This is redundant with the above, but we've already computed it
    SvnRevision* svn_revision,
    int mark,
    const QByteArray &resetTo,
    const QByteArray &comment)
  {
  if (submodule_in_repo)
      submodule_in_repo->submoduleChanged(this, branch_rule);
  
  Q_ASSERT(branch.startsWith("refs/"));
  QByteArray branchRef = branch.toUtf8();
  Branch &br = branches[branch];
  QByteArray backupCmd;
  int const revnum = svn_revision->id();
  if (br.created && br.created != revnum && !br.marks.isEmpty() && br.marks.last())
    {
    QByteArray backupBranch;
    if ((comment == "delete") && branchRef.startsWith("refs/heads/"))
      {
      backupBranch = "refs/tags/backups/" + branchRef.mid(11) + "@" + QByteArray::number(revnum);
      }
    else
      {
      backupBranch = "refs/backups/r" + QByteArray::number(revnum) + branchRef.mid(4);
      }
    Log::debug() << "backing up branch " << qPrintable(branch) << " to "
                 << qPrintable(backupBranch) << " in repository " << qPrintable(name)
                 << std::endl;
    backupCmd = "reset " + backupBranch + "\nfrom " + branchRef + "\n\n";
    }

  br.created = revnum;
  br.commits.append(revnum);
  br.marks.append(mark);

  QByteArray cmd = "reset " + branchRef + "\nfrom " + resetTo + "\n\n"
    "progress SVN r" + QByteArray::number(revnum)
    + " branch " + branch.toUtf8() + " = :" + QByteArray::number(mark)
    + " # " + comment + "\n\n";

  if (comment == "delete")
    {
    // In a single revision, we can create a branch after deleting it,
    // but not vice-versa.  Just ignore both the deletion and the
    // original creation if they occur in the same revision.
    if (resetBranches.contains(branch))
        resetBranches.remove(branch);
    else
        deletedBranches[branch].append(backupCmd).append(cmd);
    }
  else
    {
    resetBranches[branch].append(backupCmd).append(cmd);
    }
  
  return EXIT_SUCCESS;
  }

void Repository::commit()
  {
  if (deletedBranches.isEmpty() && resetBranches.isEmpty())
    {
    return;
    }
  startFastImport();
  foreach(QByteArray const& cmd, deletedBranches.values())
    fastImport.write(cmd);
  foreach(QByteArray const& cmd, resetBranches.values())
    fastImport.write(cmd);
  deletedBranches.clear();
  resetBranches.clear();
  }

Repository::Transaction *Repository::newTransaction(
    BranchRule const* branch,
    const std::string &svnprefix,
    SvnRevision* svn_revision)
  {
  return newTransaction(QString::fromStdString(git_ref_name(branch)), svnprefix, svn_revision->id());
  }

Repository::Transaction *Repository::newTransaction(
    const QString &branch,
    const std::string &svnprefix,
    int revnum)
  {
  Q_ASSERT(branch.startsWith("refs/"));
  if (!branches.contains(branch))
    {
    Log::debug() << "Creating branch '" << qPrintable(branch) << "' in repository '"
                 <<  qPrintable(name) << "'." << std::endl;
    }

  Transaction *txn = new Transaction;
  txn->repository = this;
  txn->branch = branch.toUtf8();
  txn->svnprefix = svnprefix;
  txn->datetime = 0;
  txn->revnum = revnum;

  if ((++commitCount % options.commit_interval) == 0)
    {
    startFastImport();
    // write everything to disk every 10000 commits
    fastImport.write("checkpoint\n");
    Log::debug() << "checkpoint!, marks file trunkated" << std::endl;
    }
  outstandingTransactions++;
  return txn;
  }

void Repository::forgetTransaction(Transaction *)
  {
  if (!--outstandingTransactions)
    {
    next_file_mark = maxMark;
    }
  }

void Repository::createAnnotatedTag(
    BranchRule const* branch_rule,
    const std::string &svnprefix,
    SvnRevision* svn_revision,
    const QByteArray &author,
    uint dt,
    const std::string &log)
  {
  QString ref = QString::fromStdString(git_ref_name(branch_rule));
  QString tagName = ref;
  if (tagName.startsWith("refs/tags/"))
    {
    tagName.remove(0, 10);
    }
  if (!annotatedTags.contains(tagName))
    {
    Log::debug() << "Creating annotated tag " << qPrintable(tagName)
                 << " (" << qPrintable(ref) << ')' << " in repository "
                 << qPrintable(name) << std::endl;
    }
  else
    {
    Log::debug() << "Re-creating annotated tag " << qPrintable(tagName)
                 << " in repository " << qPrintable(name) << std::endl;
    }
  AnnotatedTag &tag = annotatedTags[tagName];
  tag.supportingRef = ref;
  tag.svnprefix = svnprefix;
  tag.revnum = svn_revision->id();
  tag.author = author;
  tag.log = log;
  tag.dt = dt;
  }

void Repository::finalizeTags()
  {
  if (annotatedTags.isEmpty())
    {
    return;
    }
  std::ostream& output = Log::debug() << "Finalising tags for " << qPrintable(name) << "...";
  startFastImport();

  QHash<QString, AnnotatedTag>::ConstIterator it = annotatedTags.constBegin();
  for ( ; it != annotatedTags.constEnd(); ++it) {
    const QString &tagName = it.key();
    const AnnotatedTag &tag = it.value();

    Q_ASSERT(tag.supportingRef.startsWith("refs/"));
    std::string message = tag.log;
    if (!boost::ends_with(message, "\n"))
        message += '\n';
    if (options.add_metadata)
        message += "\n" + formatMetadataMessage(tag.svnprefix, tag.revnum, tagName.toUtf8());

    {
    QByteArray branchRef = tag.supportingRef.toUtf8();

    uint msg_len = message.size();
    QByteArray s = "progress Creating annotated tag " + tagName.toUtf8() + " from ref " + branchRef + "\n"
      + "tag " + tagName.toUtf8() + "\n"
      + "from " + branchRef + "\n"
      + "tagger " + tag.author + ' ' + QByteArray::number(tag.dt) + " +0000" + "\n"
      + "data " + QByteArray::number( msg_len ) + "\n";
    fastImport.write(s);
    }

    fastImport.write(message.c_str());
    fastImport.putChar('\n');
    if (!fastImport.waitForBytesWritten(-1))
        qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));

    // Append note to the tip commit of the supporting ref. There is no
    // easy way to attach a note to the tag itself with fast-import.
    if (options.add_metadata_notes) {
      Repository::Transaction *txn = newTransaction(tag.supportingRef, tag.svnprefix, tag.revnum);
      txn->setAuthor(tag.author);
      txn->setDateTime(tag.dt);
      txn->commitNote(formatMetadataMessage(tag.svnprefix, tag.revnum, tagName.toUtf8()), true);
      delete txn;

      if (!fastImport.waitForBytesWritten(-1))
          qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));
    }

    output << ' ' << qPrintable(tagName) << std::flush;
  }

  while (fastImport.bytesToWrite())
      if (!fastImport.waitForBytesWritten(-1))
          qFatal("Failed to write to process: %s", qPrintable(fastImport.errorString()));
  output << std::endl;
  }

void Repository::startFastImport()
  {
  processCache.touch(this);

  if (fastImport.state() == QProcess::NotRunning) {
    if (processHasStarted)
        qFatal("git-fast-import has been started once and crashed?");
    processHasStarted = true;

    // start the process
    QString marksFile = marksFileName(name);
    QStringList marksOptions;
    marksOptions << "--import-marks=" + marksFile;
    marksOptions << "--export-marks=" + marksFile;
    marksOptions << "--force";

    fastImport.setStandardOutputFile(logFileName(name), QIODevice::Append);
    fastImport.setProcessChannelMode(QProcess::MergedChannels);

    if (!options.dry_run) {
      fastImport.start("git", QStringList() << "fast-import" << marksOptions);
    } else {
      fastImport.start("/bin/cat", QStringList());
    }
    fastImport.waitForStarted(-1);

    reloadBranches();
  }
  }

std::string Repository::formatMetadataMessage(const std::string &svnprefix, int revnum, const QByteArray &tag)
  {
  std::string msg = "svn path=" + svnprefix + "; revision=" + to_string(revnum);
  if (!tag.isEmpty())
      msg += "; tag=" + std::string(tag.data(), tag.length());
  msg += "\n";
  return msg;
  }

bool Repository::branchExists(const QString& branch) const
  {
  return branches.contains(branch);
  }

const QByteArray Repository::branchNote(const QString& branch) const
  {
  return branches.value(branch).note;
  }

void Repository::setBranchNote(const QString& branch, const QByteArray& noteText)
  {
  if (branches.contains(branch))
      branches[branch].note = noteText;
  }

Repository::Transaction::~Transaction()
  {
  repository->forgetTransaction(this);
  }

void Repository::Transaction::setAuthor(const QByteArray &a)
  {
  author = a;
  }

void Repository::Transaction::setDateTime(uint dt)
  {
  datetime = dt;
  }

void Repository::Transaction::setLog(const std::string &l)
  {
  log = l;
  }

void Repository::Transaction::noteCopyFromBranch(
    const QString &branchFrom,
    int branchRevNum)
  {
  Q_ASSERT(branchFrom.startsWith("refs/"));
  if (branch == branchFrom)
    {
    Log::warn() << "Cannot merge inside a branch" << " in repository " << qPrintable(repository->name) << std::endl;
    return;
    }

  static QByteArray dummy;
  int mark = repository->markFrom(branchFrom, branchRevNum, dummy);
  Q_ASSERT(dummy.isEmpty());

  if (mark == -1)
    {
    Log::warn() << qPrintable(branch) << " is copying from branch " << qPrintable(branchFrom)
                << " but the latter doesn't exist. Continuing, assuming the files exist"
                << " in repository " << qPrintable(repository->name) << std::endl;
    }
  else if (mark == 0)
    {
    Log::warn() << "Unknown revision r" << branchRevNum << ". Continuing, assuming the files exist"
                << " in repository " << qPrintable(repository->name) << std::endl;
    }
  else
    {
    Log::debug() << "repository " << qPrintable(repository->name) << " branch " << qPrintable(branch)
                 << " has some files copied from " << qPrintable(branchFrom) << "@" << branchRevNum << std::endl;
    if (!merges.contains(mark))
      {
      merges.append(mark);
      Log::debug() << "adding " << qPrintable(branchFrom) << "@" << branchRevNum << " : " << mark
                   << " as a merge point" << " in repository " << qPrintable(repository->name) << std::endl;
      }
    else
      {
      Log::debug() << "merge point already recorded" << " in repository "
                   << qPrintable(repository->name) << std::endl;
      }
    }
  }

void Repository::Transaction::deleteFile(const QString &path)
  {
  QString pathNoSlash = repository->prefix + path;
  if(pathNoSlash.endsWith('/'))
      pathNoSlash.chop(1);
  deletedFiles.append(pathNoSlash);
  }

QIODevice *Repository::Transaction::addFile(const QString &path, int mode, qint64 length)
  {
  int mark = repository->next_file_mark--;

  // in case the two mark allocations meet, we might as well just abort
  Q_ASSERT(mark > repository->last_commit_mark + 1);
  
  Q_ASSERT(!(repository->prefix + path.toUtf8()).toStdString().empty());

  if (modifiedFiles.capacity() == 0)
      modifiedFiles.reserve(2048);
  modifiedFiles.append("M ");
  modifiedFiles.append(QByteArray::number(mode, 8));
  modifiedFiles.append(" :");
  modifiedFiles.append(QByteArray::number(mark));
  modifiedFiles.append(' ');
  modifiedFiles.append(repository->prefix + path.toUtf8());
  modifiedFiles.append("\n");

  if (!options.dry_run) {
    repository->startFastImport();
    repository->fastImport.writeNoLog("blob\nmark :");
    repository->fastImport.writeNoLog(QByteArray::number(mark));
    repository->fastImport.writeNoLog("\ndata ");
    repository->fastImport.writeNoLog(QByteArray::number(length));
    repository->fastImport.writeNoLog("\n", 1);
  }

  return &repository->fastImport;
  }

void Repository::Transaction::commitNote(const std::string &noteText, bool append, const QByteArray &commit)
  {
  Q_ASSERT(branch.startsWith("refs/"));
  QByteArray branchRef = branch;
  const QByteArray &commitRef = commit.isNull() ? branchRef : commit;
  QByteArray message = "Adding Git note for current " + commitRef + "\n";
  QByteArray text(noteText.data(), noteText.length());

  if (append && commit.isNull() &&
    repository->branchExists(branch) &&
    !repository->branchNote(branch).isEmpty())
    {
    text = repository->branchNote(branch) + text;
    message = "Appending Git note for current " + commitRef + "\n";
    }

  QByteArray s("");
  s.append("commit refs/notes/commits\n");
  s.append("mark :" + QByteArray::number(maxMark + 1) + "\n");
  s.append("committer " + QString::fromUtf8(author) + " " + QString::number(datetime) + " +0000" + "\n");
  s.append("data " + QString::number(message.length()) + "\n");
  s.append(message + "\n");
  s.append("N inline " + commitRef + "\n");
  s.append("data " + QString::number(text.length()) + "\n");
  s.append(text + "\n");
  repository->fastImport.write(s);

  if (commit.isNull()) {
    repository->setBranchNote(QString::fromUtf8(branch), text);
  }
  }

void Repository::Transaction::commit()
  {
  repository->startFastImport();

  // We might be tempted to use the SVN revision number as the fast-import commit mark.
  // However, a single SVN revision can modify multple branches, and thus lead to multiple
  // commits in the same repo.  So, we need to maintain a separate commit mark counter.
  int mark = ++repository->last_commit_mark;

  // in case the two mark allocations meet, we might as well just abort
  Q_ASSERT(mark < repository->next_file_mark - 1);

  // create the commit message
  std::string message = log;
  if (!boost::ends_with(message, "\n"))
      message += '\n';
  if (options.add_metadata)
      message += "\n" + Repository::formatMetadataMessage(svnprefix, revnum);

  int parentmark = 0;
  Branch &br = repository->branches[branch];
  if (br.created && !br.marks.isEmpty() && br.marks.last()) {
    parentmark = br.marks.last();
  } else {
    if (repository->incremental)
      {
      Log::warn() << "Branch " << qPrintable(branch) << " in repository "
                  << qPrintable(repository->name) << " doesn't exist at revision "
                  << revnum << " -- did you resume from the wrong revision?" << std::endl;
      }
    br.created = revnum;
  }
  br.commits.append(revnum);
  br.marks.append(mark);

  Q_ASSERT(branch.startsWith("refs/"));
  QByteArray branchRef = branch;

  QByteArray s("");
  s.append("commit " + branchRef + "\n");
  s.append("mark :" + QByteArray::number(mark) + "\n");
  s.append("committer " + QString::fromUtf8(author) + " " + QString::number(datetime) + " +0000" + "\n");
  s.append("data " + QString::number(message.length()) + "\n");
  s.append(QByteArray(message.c_str()) + "\n");
  repository->fastImport.write(s);

  // note some of the inferred merges
  QByteArray desc = "";
  int i = !!parentmark;	// if parentmark != 0, there's at least one parent

  if(log.find("This commit was manufactured by cvs2svn") != std::string::npos && merges.count() > 1) {
    qSort(merges);
    repository->fastImport.write("merge :" + QByteArray::number(merges.last()) + "\n");
    merges.pop_back();
    Log::debug() << "Discarding all but the highest merge point "
                 << "as a workaround for cvs2svn created branch/tag. "
      //<< "Discarded marks: " << merges
      ;
  } else {
    foreach (const int merge, merges) {
      if (merge == parentmark) {
        Log::debug() << "Skipping marking " << merge << " as a merge point as it matches the parent"
                     << " in repository " << qPrintable(repository->name) << std::endl;
        continue;
      }

      if (++i > 16) {
        // FIXME: options:
        //   (1) ignore the 16 parent limit
        //   (2) don't emit more than 16 parents
        //   (3) create another commit on branch to soak up additional parents
        // we've chosen option (2) for now, since only artificial commits
        // created by cvs2svn seem to have this issue
        Log::warn() << "too many merge parents" << " in repository "
                    << qPrintable(repository->name) << std::endl;
        break;
      }

      QByteArray m = " :" + QByteArray::number(merge);
      desc += m;
      repository->fastImport.write("merge" + m + "\n");
    }
  }
  // write the file deletions
  if (deletedFiles.contains(""))
      repository->fastImport.write("deleteall\n");
  else
      foreach (QString df, deletedFiles)
        repository->fastImport.write("D " + df.toUtf8() + "\n");

  // write the file modifications
  repository->fastImport.write(modifiedFiles);

  repository->fastImport.write("\nprogress SVN r" + QByteArray::number(revnum)
    + " branch " + branch + " = :" + QByteArray::number(mark)
    + (desc.isEmpty() ? "" : " # merge from") + desc
    + "\n\n");
  Log::trace() << deletedFiles.count() + modifiedFiles.count('\n')
               << " modifications from SVN " << svnprefix.data() << " to " << qPrintable(repository->name)
               << '/' << branch.data() << std::endl;

  // Commit metadata note if requested
  if (options.add_metadata_notes)
      commitNote(Repository::formatMetadataMessage(svnprefix, revnum), false);

  while (repository->fastImport.bytesToWrite())
      if (!repository->fastImport.waitForBytesWritten(-1))
          qFatal("Failed to write to process: %s for repository %s",
            qPrintable(repository->fastImport.errorString()), qPrintable(repository->name));
  }

void Repository::submoduleChanged(Repository const* submodule, BranchRule const* branch_rule)
  {
  }
