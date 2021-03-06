/*
    Copyright (C) 2019-2020  Selwin van Dijk

    This file is part of signalbackup-tools.

    signalbackup-tools is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    signalbackup-tools is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with signalbackup-tools.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "signalbackup.ih"

void SignalBackup::importThread(SignalBackup *source, long long int thread)
{
  std::cout << __FUNCTION__ << std::endl;

  if ((d_databaseversion >= 33 && source->d_databaseversion < 33) ||
      (d_databaseversion < 33 && source->d_databaseversion >= 33) ||
      (d_databaseversion >= 27 && source->d_databaseversion < 27) ||
      (d_databaseversion < 27 && source->d_databaseversion >= 27))
  {
    std::cout << "Source and target database at incompatible versions" << std::endl;
    return;
  }

  // crop the source db to the specified thread
  source->cropToThread(thread);

  long long int targetthread = -1;
  SqliteDB::QueryResults results;
  if (d_databaseversion < 24) // old database version
  {
    // get targetthread from source thread id (source.thread_id->source.recipient_id->target.thread_id
    source->d_database.exec("SELECT recipient_ids FROM thread WHERE _id = ?", thread, &results);
    if (results.rows() != 1 || results.columns() != 1 ||
        !results.valueHasType<std::string>(0, 0))
    {
      std::cout << "Failed to get recipient id from source database" << std::endl;
      return;
    }
    std::string recipient_id = results.getValueAs<std::string>(0, 0);
    targetthread = getThreadIdFromRecipient(recipient_id);
  }
  else // new database version
  {
    // get targetthread from source thread id (source.thread_id->source.recipient_id->source.recipient.phone/group_id->target.thread_id
    source->d_database.exec("SELECT COALESCE(phone,group_id) FROM recipient WHERE _id IS (SELECT recipient_ids FROM thread WHERE _id = ?)", thread, &results);
    if (results.rows() != 1 || results.columns() != 1 ||
        !results.valueHasType<std::string>(0, 0))
    {
      std::cout << "Failed to get phone/group_id from source database" << std::endl;
      return;
    }
    std::string phone_or_group = results.getValueAs<std::string>(0, 0);
    d_database.exec("SELECT _id FROM recipient WHERE COALESCE(phone,group_id) = ?", phone_or_group, &results);
    if (results.rows() != 1 || results.columns() != 1 ||
        !results.valueHasType<long long int>(0, 0))
      std::cout << "Failed to find recipient._id matching phone/group_id in target database" << std::endl;
    else
    {
      long long int recipient_id = results.getValueAs<long long int>(0, 0);
      targetthread = getThreadIdFromRecipient(bepaald::toString(recipient_id));
    }
  }

  // delete double megaphones
  if (d_database.containsTable("megaphone") && source->d_database.containsTable("megaphone"))
  {
    SqliteDB::QueryResults res;
    d_database.exec("SELECT event FROM megaphone", &res);

    std::cout << "  Deleting " << res.rows() << " existing megaphones" << std::endl;

    for (uint i = 0; i < res.rows(); ++i)
      source->d_database.exec("DELETE FROM megaphone WHERE event = ?", res.getValueAs<std::string>(i, 0));
  }

  // the target will have its own job_spec etc...
  source->d_database.exec("DELETE FROM job_spec");
  source->d_database.exec("DELETE FROM push");
  source->d_database.exec("DELETE FROM constraint_spec"); // has to do with job_spec, references it...
  source->d_database.exec("DELETE FROM dependency_spec"); // has to do with job_spec, references it...
  source->d_database.exec("VACUUM");

  // make sure all id's are unique
  long long int offsetthread = getMaxUsedId("thread") + 1 - source->getMinUsedId("thread");
  long long int offsetsms = getMaxUsedId("sms") + 1 - source->getMinUsedId("sms");
  long long int offsetmms = getMaxUsedId("mms") + 1 - source->getMinUsedId("mms");
  long long int offsetpart = getMaxUsedId("part") + 1 - source->getMinUsedId("part");
  long long int offsetrecipient = getMaxUsedId((d_databaseversion < 24) ? "recipient_preferences" : "recipient") + 1 - source->getMinUsedId((d_databaseversion < 24) ? "recipient_preferences" : "recipient");
  long long int offsetgroups = getMaxUsedId("groups") + 1 - source->getMinUsedId("groups");
  long long int offsetidentities = getMaxUsedId("identities") + 1 - source->getMinUsedId("identities");
  long long int offsetgroup_receipts = getMaxUsedId("group_receipts") + 1 - source->getMinUsedId("group_receipts");
  long long int offsetdrafts = getMaxUsedId("drafts") + 1 - source->getMinUsedId("drafts");
  long long int offsetsticker = getMaxUsedId("sticker") + 1 - source->getMinUsedId("sticker");
  long long int offsetmegaphone = getMaxUsedId("megaphone") + 1 - source->getMinUsedId("megaphone");
  source->makeIdsUnique(offsetthread, offsetsms, offsetmms, offsetpart, offsetrecipient, offsetgroups, offsetidentities, offsetgroup_receipts, offsetdrafts, offsetsticker, offsetmegaphone);

  // merge into existing thread, set the id on the sms, mms, and drafts
  // drop the recipient_preferences, identities and thread tables, they are already in the target db
  if (targetthread > -1)
  {
    std::cout << "  Found existing thread for this recipient in target database, merging into thread " << targetthread << std::endl;
    source->d_database.exec("UPDATE sms SET thread_id = ?", targetthread);
    source->d_database.exec("UPDATE mms SET thread_id = ?", targetthread);
    source->d_database.exec("UPDATE drafts SET thread_id = ?", targetthread);

    // see below for comment explaing this function
    if (d_databaseversion >= 24)
    {
      d_database.exec("SELECT _id, COALESCE(phone,group_id) AS ident FROM recipient", &results);
      for (uint i = 0; i < results.rows(); ++i)
        if (results.valueHasType<std::string>(i, "ident"))
          source->updateRecipientId(results.getValueAs<long long int>(i, "_id"), results.getValueAs<std::string>(i, "ident"));
    }

    source->d_database.exec("DROP TABLE thread");
    source->d_database.exec("DROP TABLE identities");
    source->d_database.exec((d_databaseversion < 24) ? "DROP TABLE recipient_preferences" : "DROP TABLE recipient");
    source->d_database.exec("DROP TABLE groups");
    source->d_avatars.clear();
  }
  else
  {
    // check identities and recepient prefs for presence of values, they may be there (even though no
    // thread was found (for example via a group chat or deleted thread))
    // get identities from target, drop all rows from source that are already present
    if (d_databaseversion < 24)
    {
      d_database.exec("SELECT address FROM identities", &results); // address == phonenumber/__text_secure_group
      for (uint i = 0; i < results.rows(); ++i)
        if (results.header(0) == "address" && results.valueHasType<std::string>(i, 0))
          source->d_database.exec("DELETE FROM identities WHERE ADDRESS = '" + results.getValueAs<std::string>(i, 0) + "'");
    }
    else
    {
      // get all phonenums/groups_ids for all in identities
      d_database.exec("SELECT COALESCE(phone,group_id) AS ident FROM recipient WHERE _id IN (SELECT address FROM identities)", &results);
      for (uint i = 0; i < results.rows(); ++i)
        if (results.header(0) == "ident" && results.valueHasType<std::string>(i, 0))
          source->d_database.exec("DELETE FROM identities WHERE address IN (SELECT _id FROM recipient WHERE COALESCE(phone,group_id) = '" + results.getValueAs<std::string>(i, 0) + "')");
    }

    // get recipient(_preferences) from target, drop all rows from source that are allready present
    if (d_databaseversion < 24)
    {
      d_database.exec("SELECT recipient_ids FROM recipient_preferences", &results);
      for (uint i = 0; i < results.rows(); ++i)
        if (results.header(0) == "recipient_ids" && results.valueHasType<std::string>(i, 0))
          source->d_database.exec("DELETE FROM recipient_preferences WHERE recipient_ids = '" + results.getValueAs<std::string>(i, 0) + "'");
    }
    else
    {
      d_database.exec("SELECT _id,COALESCE(phone,group_id) AS ident FROM recipient", &results);
      for (uint i = 0; i < results.rows(); ++i)
        if (results.valueHasType<std::string>(i, "ident"))
        {
          // if the recipient is already in target, we are going to delete it from
          // source, to prevent doubles. However, many tables refer to the recipient._id
          // which was made unique above. If we just delete the doubles (by phone/group_id,
          // and in the future probably uuid), the fields in other tables will point
          // to random or non-existing recipients, so we need to map them:
          source->updateRecipientId(results.getValueAs<long long int>(i, "_id"), results.getValueAs<std::string>(i, "ident"));

          // now drop the already present recipient from source.
          source->d_database.exec("DELETE FROM recipient WHERE COALESCE(phone,group_id) = '" + results.getValueAs<std::string>(i, "ident") + "'");
        }
    }

    // even though the source was cropped to single thread, and this thread was not in target, avatar might still already be in target
    // because contact (and avatar) might be present in group in source, and only as one-on-one in target

    bool erased = true;
    while (erased)
    {
      erased = false;
      for (std::vector<std::pair<std::string, std::unique_ptr<AvatarFrame>>>::iterator sourceav = source->d_avatars.begin(); sourceav != source->d_avatars.end(); ++sourceav)
      {
        for (std::vector<std::pair<std::string, std::unique_ptr<AvatarFrame>>>::iterator targetav = d_avatars.begin(); targetav != d_avatars.end(); ++targetav)
          if (sourceav->first == targetav->first)
          {
            source->d_avatars.erase(sourceav);
            erased = true;
            break;
          }
        if (erased)
          break;
      }
    }
  }

  // now import the source tables into target,

  // get tables
  std::string q("SELECT sql, name, type FROM sqlite_master");
  source->d_database.exec(q, &results);
  std::vector<std::string> tables;
  for (uint i = 0; i < results.rows(); ++i)
  {
    if (!results.valueHasType<std::nullptr_t>(i, 0))
    {
      if (results.valueHasType<std::string>(i, 1) &&
          (results.getValueAs<std::string>(i, 1) != "sms_fts" &&
           results.getValueAs<std::string>(i, 1).find("sms_fts") == 0))
        ;//std::cout << "Skipping " << results[i][1].second << " because it is smsftssecrettable" << std::endl;
      else if (results.valueHasType<std::string>(i, 1) &&
               (results.getValueAs<std::string>(i, 1) != "mms_fts" &&
                results.getValueAs<std::string>(i, 1).find("mms_fts") == 0))
        ;//std::cout << "Skipping " << results[i][1].second << " because it is smsftssecrettable" << std::endl;
      else
        if (results.valueHasType<std::string>(i, 2) && results.getValueAs<std::string>(i, 2) == "table")
          tables.emplace_back(results.getValueAs<std::string>(i, 1));
    }
  }

  // write contents of tables
  for (std::string const &table : tables)
  {
    if (table == "signed_prekeys" ||
        table == "one_time_prekeys" ||
        table == "sessions" ||
        STRING_STARTS_WITH(table, "sms_fts") ||
        STRING_STARTS_WITH(table, "mms_fts") ||
        STRING_STARTS_WITH(table, "sqlite_"))
      continue;
    std::cout << "Importing statements from source table '" << table << "'...";
    source->d_database.exec("SELECT * FROM " + table, &results);
    std::cout << results.rows() << " entries..." << std::endl;

    // check if source contains columns not existing in target
    // even though target is newer, a fresh install would not have
    // created dropped columns that may still be present in
    // source database;
    uint idx = 0;
    while (idx < results.headers().size())
    {
      if (!d_database.tableContainsColumn(table, results.headers()[idx]))
      {
        std::cout << "NOTE: dropping column '" << table << "." << results.headers()[idx] << "' from source : Column not present in target database" << std::endl;
        if (results.removeColumn(idx++))
          continue;
      }
      ++idx;
    }

    for (uint i = 0; i < results.rows(); ++i)
    {
      // if (table == "identities")
      // {
      //   std::cout << "Trying to add: ";
      //   for (uint j = 0; j < results.columns(); ++j)
      //     std::cout << results.valueAsString(i, j) << " ";
      //   std::cout << std::endl;
      // }
      SqlStatementFrame newframe = buildSqlStatementFrame(table, results.headers(), results.row(i));
      d_database.exec(newframe.bindStatement(), newframe.parameters());
    }
  }

  // and copy avatars and attachments.
  for (auto &att : source->d_attachments)
    d_attachments.emplace(std::move(att));

  for (auto &av : source->d_avatars)
    d_avatars.emplace_back(std::move(av));

  // update thread snippet and date and count
  updateThreadsEntries();

}
