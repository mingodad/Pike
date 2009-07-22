//
// Basic filesystem monitor.
//
// $Id: basic.pike,v 1.17 2009/07/22 15:08:28 grubba Exp $
//
// 2009-07-09 Henrik Grubbström
//

//! Basic filesystem monitor.
//!
//! This module is intended to be used for incremental scanning of
//! a filesystem.

//! The default maximum number of seconds between checks of directories
//! in seconds.
//!
//! This value is multiplied with @[default_file_interval_factor] to
//! get the corresponding default maximum number of seconds for files.
//!
//! The value can be changed by calling @[create()].
//!
//! The value can be overridden for individual files or directories
//! by calling @[monitor()].
//!
//! Overload this constant to change the default.
protected constant default_max_dir_check_interval = 60;

//! The default factor to multiply @[default_max_dir_check_interval]
//! with to get the maximum number of seconds between checks of files.
//!
//! The value can be changed by calling @[create()].
//!
//! The value can be overridden for individual files or directories
//! by calling @[monitor()].
//!
//! Overload this constant to change the default.
protected constant default_file_interval_factor = 5;

//! The default minimum number of seconds without changes for a change
//! to be regarded as stable (see @[stable_data_change()].
protected constant default_stable_time = 5;

protected int max_dir_check_interval = default_max_dir_check_interval;
protected int file_interval_factor = default_file_interval_factor;
protected int stable_time = default_stable_time;

// Callbacks

//! File content changed callback.
//!
//! @param path
//!   Path of the file which has had content changed.
//!
//! This function is called when a change has been detected for a
//! monitored file.
//!
//! Called by @[check()] and @[check_monitor()].
//!
//! Overload this to do something useful.
void data_changed(string path);

//! File attribute changed callback.
//!
//! @param path
//!   Path of the file or directory which has changed attributes.
//!
//! @param st
//!   Status information for @[path] as obtained by @expr{file_stat(path, 1)@}.
//!
//! This function is called when a change has been detected for an
//! attribute for a monitored file or directory.
//!
//! Called by @[check()] and @[check_monitor()].
//!
//! @note
//!   If there is a @[data_changed()] callback, it may supersede this
//!   callback if the file content also has changed.
//!
//! Overload this to do something useful.
void attr_changed(string path, Stdio.Stat st);

//! File existance callback.
//!
//! @param path
//!   Path of the file or directory.
//!
//! @param st
//!   Status information for @[path] as obtained by @expr{file_stat(path, 1)@}.
//!
//! This function is called during initialization for all monitored paths,
//! and subpaths for monitored directories. It represents the initial state
//! for the monitor.
//!
//! @note
//!   For directories, @[file_created()] will be called for the subpaths
//!   before the call for the directory itself. This can be used to detect
//!   when the initialization for a directory is finished.
//!
//! Called by @[check()] and @[check_monitor()] the first time a monitored
//! path is checked (and only if it exists).
//!
//! Overload this to do something useful.
void file_exists(string path, Stdio.Stat st);

//! File creation callback.
//!
//! @param path
//!   Path of the new file or directory.
//!
//! @param st
//!   Status information for @[path] as obtained by @expr{file_stat(path, 1)@}.
//!
//! This function is called when either a monitored path has started
//! existing, or when a new file or directory has been added to a
//! monitored directory.
//!
//! Called by @[check()] and @[check_monitor()].
//!
//! Overload this to do something useful.
void file_created(string path, Stdio.Stat st);

//! File deletion callback.
//!
//! @param path
//!   Path of the new file or directory that has been deleted.
//!
//! This function is called when either a monitored path has stopped
//! to exist, or when a file or directory has been deleted from a
//! monitored directory.
//!
//! Called by @[check()] and @[check_monitor()].
//!
//! Overload this to do something useful.
void file_deleted(string path);

//! Stable change callback.
//!
//! @param path
//!   Path of the file or directory that has stopped changing.
//!
//! @param st
//!   Status information for @[path] as obtained by @expr{file_stat(path, 1)@}.
//!
//! This function is called when previous changes to @[path] are
//! considered "stable".
//!
//! "Stable" in this case means that there have been no detected
//! changes for at lease @[stable_time] seconds.
//!
//! Called by @[check()] and @[check_monitor()].
//!
//! Overload this to do something useful.
void stable_data_change(string path, Stdio.Stat st);

//! Flags for @[Monitor]s.
enum MonitorFlags {
  MF_RECURSE = 1,
  MF_AUTO = 2,
  MF_INITED = 4,
};

//! Monitoring information for a single filesystem path.
//!
//! @seealso
//!   @[monitor()]
protected class Monitor(string path,
			MonitorFlags flags,
			int max_dir_check_interval,
			int file_interval_factor,
			int stable_time)
{
  int next_poll;
  Stdio.Stat st;
  int last_change = 0x7fffffff;	// Future...
  array(string) files;

  int `<(mixed m) { return next_poll < m; }
  int `>(mixed m) { return next_poll > m; }

  protected string _sprintf(int c)
  {
    return sprintf("Monitor(%O, %O, next: %s, st: %O)",
		   path, flags, ctime(next_poll) - "\n", st);
  }

  //! Calculate a suitable time for the next poll of this monitor.
  //!
  //! @param st
  //!   New stat for the monitor.
  //!
  //! This function is called by @[check()] to schedule the
  //! next check.
  protected void update(Stdio.Stat st)
  {
    int delta = max_dir_check_interval || global::max_dir_check_interval;
    this_program::st = st;
    if (!st || !st->isdir) {
      delta *= file_interval_factor || global::file_interval_factor;
    }
    if (!next_poll) {
      // Attempt to distribute polls evenly at startup.
      delta = 1 + random(delta);
    }
    if (st) {
      int d = 1 + ((time(1) - st->mtime)>>4);
      if (d < 0) d = max_dir_check_interval || global::max_dir_check_interval;
      if (d < delta) delta = d;
      d = 1 + ((time(1) - st->ctime)>>4);
      if (d < 0) d = max_dir_check_interval || global::max_dir_check_interval;
      if (d < delta) delta = d;
    }
    next_poll = time(1) + (delta || 1);
    monitor_queue->adjust(this);
  }

  //! Check for changes.
  //!
  //! @param flags
  //!   @int
  //!     @value 0
  //!       Don't recurse.
  //!     @value 1
  //!       Check all monitors for the entire subtree rooted in @[m].
  //!   @endint
  //!
  //! This function is called by @[check()] for the @[Monitor]s
  //! it considers need checking. If it detects any changes an
  //! appropriate callback will be called.
  //!
  //! @returns
  //!   Returns @expr{1@} if a change was detected and @expr{0@} (zero)
  //!   otherwise.
  //!
  //! @note
  //!   Any callbacks will be called from the same thread as the one
  //!   calling @[check_monitor()].
  //!
  //! @note
  //!   The return value can not be trusted to return @expr{1@} for all
  //!   detected changes in recursive mode.
  //!
  //! @seealso
  //!   @[check()], @[data_changed()], @[attr_changed()], @[file_created()],
  //!   @[file_deleted()], @[stable_data_change()]
  int(0..1) check(MonitorFlags|void flags)
  {
    // werror("Checking monitor %O...\n", this);
    Stdio.Stat st = file_stat(path, 1);
    Stdio.Stat old_st = this_program::st;
    int orig_flags = this_program::flags;
    this_program::flags |= MF_INITED;
    update(st);
    if (!(orig_flags & MF_INITED)) {
      // Initialize.
      if (st) {
	if (st->isdir) {
	  array(string) files = get_dir(path);
	  this_program::files = files;
	  foreach(files, string file) {
	    file = Stdio.append_path(path, file);
	    if (monitors[file]) {
	      // There's already a monitor for the file.
	      // Assume it has already notified about existance.
	      continue;
	    }
	    if (this_program::flags & MF_RECURSE) {
	      monitor(file, orig_flags | MF_AUTO,
		      max_dir_check_interval,
		      file_interval_factor,
		      stable_time);
	      check_monitor(monitors[file]);
	    } else if (file_exists) {
	      file_exists(file, file_stat(file, 1));
	    }
	  }
	}
	// Signal file_exists for path as an end marker.
	if (file_exists) {
	  file_exists(path, st);
	}
      }
      return 1;
    }
    if (!st) {
      if (old_st) {
	if (this_program::flags & MF_AUTO) {
	  m_delete(monitors, path);
	  release_monitor(this);
	}
	if (file_deleted) {
	  file_deleted(path);
	}
	return 1;
      }
      return 0;
    }
    if (!old_st) {
      last_change = time(1);
      if (file_created) {
	file_created(path, st);
      }
      return 1;
    }
    if ((st->mtime != old_st->mtime) || (st->ctime != old_st->ctime) ||
	(st->size != old_st->size)) {
      last_change = time(1);
      if (st->isdir) {
	array(string) files = get_dir(path);
	array(string) new_files = files;
	array(string) deleted_files = ({});
	if (this_program::files) {
	  new_files -= this_program::files;
	  deleted_files = this_program::files - files;
	}
	this_program::files = files;
	foreach(new_files, string file) {
	  file = Stdio.append_path(path, file);
	  Monitor m2 = monitors[file];
	  mixed err = catch {
	      if (m2) {
		// We have a separate monitor on the created file.
		// Let it handle the notification.
		m2->check(flags);
	      }
	    };
	  if (this_program::flags & MF_RECURSE) {
	    monitor(file, orig_flags | MF_AUTO,
		    max_dir_check_interval,
		    file_interval_factor,
		    stable_time);
	    monitors[file]->check();
	  } else if (!m2 && file_created) {
	    file_created(file, file_stat(file, 1));
	  }
	}
	foreach(deleted_files, string file) {
	  file = Stdio.append_path(path, file);
	  Monitor m2 = monitors[file];
	  mixed err = catch {
	      if (m2) {
		// We have a separate monitor on the deleted file.
		// Let it handle the notification.
		m2->check(flags);
	      }
	    };
	  if (this_program::flags & MF_RECURSE) {
	    // The monitor for the file has probably removed itself,
	    // or the user has done it by hand, in either case we
	    // don't need to do anything more here.
	  } else if (!m2 && file_deleted) {
	    file_deleted(file);
	  }
	  if (err) throw(err);
	}
	if (flags & MF_RECURSE) {
	  // Check the remaining files in the directory.
	  foreach(((files - new_files) - deleted_files), string file) {
	    file = Stdio.append_path(path, file);
	    Monitor m2 = monitors[file];
	    if (m2) {
	      m2->check(flags);
	    }
	  }
	}
	if (sizeof(new_files) || sizeof(deleted_files)) return 1;
      } else {
	if (data_changed) {
	  data_changed(path);
	  return 1;
	}
	if (attr_changed) {
	  attr_changed(path, st);
	}
	return 1;
      }
    }
    if ((flags & MF_RECURSE) && (st->isdir)) {
      // Check the files in the directory.
      foreach(files, string file) {
	file = Stdio.append_path(path, file);
	Monitor m2 = monitors[file];
	if (m2) {
	  m2->check(flags);
	}
      }
    }
    if (last_change < time(1) - (stable_time || global::stable_time)) {
      last_change = 0x7fffffff;
      if (stable_data_change) {
	stable_data_change(path, st);
      }
      return 1;
    }
    return 0;
  }
}

//! Mapping from monitored path to corresponding @[Monitor].
//!
//! The paths are normalized to @expr{combine_path(path, ".")@},
//! i.e. no trailing slashes.
//!
//! @note
//!   All filesystems are handled as if case-sensitive. This should
//!   not be a problem for case-insensitive filesystems as long as
//!   case is maintained.
protected mapping(string:Monitor) monitors = ([]);

//! Heap containing all active @[Monitor]s.
//!
//! The heap is sorted on @[Monitor()->next_poll].
protected ADT.Heap monitor_queue = ADT.Heap();

//! Create a new monitor.
//!
//! @param max_dir_check_interval
//!   Override of @[default_max_dir_check_interval].
//!
//! @param file_interval_factor
//!   Override of @[default_file_interval_factor].
//!
//! @param stable_time
//!   Override of @[default_stable_time].
protected void create(int|void max_dir_check_interval,
		      int|void file_interval_factor,
		      int|void stable_time)
{
  if (max_dir_check_interval > 0) {
    this_program::max_dir_check_interval = max_dir_check_interval;
  }
  if (file_interval_factor > 0) {
    this_program::file_interval_factor = file_interval_factor;
  }
  if (stable_time > 0) {
    this_program::stable_time = stable_time;
  }
  clear();
}

//! Clear the set of monitored files and directories.
//!
//! @note
//!   Due to circular datastructures, it's recomended
//!   to call this function prior to discarding the object.
void clear()
{
  monitors = ([]);
  monitor_queue = ADT.Heap();
}

//! Calculate a suitable time for the next poll of this monitor.
//!
//! @param m
//!   Monitor to update.
//!
//! @param st
//!   New stat for the monitor.
//!
//! This function is called by @[check_monitor()] to schedule the
//! next check.
protected void update_monitor(Monitor m, Stdio.Stat st)
{
  m->update(st);
}

//! Release a single @[Monitor] from monitoring.
//!
//! @seealso
//!   @[release()]
protected void release_monitor(Monitor m)
{
  m->next_poll = -1000;
  monitor_queue->adjust(m);
  while (monitor_queue->peek() < 0) {
    monitor_queue->pop();
  }
}

//! Create a new @[Monitor] for a @[path].
//!
//! This function is called by @[monitor()] to create a new @[Monitor]
//! object.
//!
//! The default implementation just calls @[Monitor()] with the same
//! arguments.
//!
//! @seealso
//!   @[monitor()]
protected Monitor monitor_factory(string path, MonitorFlags|void flags,
				  int(0..)|void max_dir_check_interval,
				  int(0..)|void file_interval_factor,
				  int(0..)|void stable_time)
{
  return Monitor(path, flags, max_dir_check_interval,
		 file_interval_factor, stable_time);
}


//! Register a @[path] for monitoring.
//!
//! @param path
//!   Path to monitor.
//!
//! @param flags
//!   @int
//!     @value 0
//!       Don't recurse.
//!     @value 1
//!       Monitor the entire subtree, and any directories
//!       or files that may appear later.
//!     @value 3
//!       Monitor the entire subtree, and any directories
//!       or files that may appear later. Remove the monitor
//!       automatically when @[path] is deleted.
//!   @endint
//!
//! @param max_dir_check_interval
//!   Override of @[default_max_dir_check_interval] for this path
//!   or subtree.
//!
//! @param file_interval_factor
//!   Override of @[default_file_interval_factor] for this path
//!   or subtree.
//!
//! @param stable_time
//!   Override of @[default_stable_time] for this path
//!   or subtree.
//!
//! @seealso
//!   @[release()]
void monitor(string path, MonitorFlags|void flags,
	     int(0..)|void max_dir_check_interval,
	     int(0..)|void file_interval_factor,
	     int(0..)|void stable_time)
{
  path = combine_path(path, ".");
  Monitor m = monitors[path];
  if (m) {
    if (!(flags & MF_AUTO)) {
      // The new monitor is added by hand.
      // Adjust the monitor.
      m->flags = flags;
      m->max_dir_check_interval = max_dir_check_interval;
      m->file_interval_factor = file_interval_factor;
      m->stable_time = stable_time;
      m->next_poll = 0;
      monitor_queue->adjust(m);
    }
    // For the other cases there's no need to do anything,
    // since we can keep the monitor as-is.
  } else {
    m = monitor_factory(path, flags, max_dir_check_interval,
			file_interval_factor, stable_time);
    monitors[path] = m;
    monitor_queue->push(m);
  }
}

//! Release a @[path] from monitoring.
//!
//! @param path
//!   Path to stop monitoring.
//!
//! @param flags
//!   @int
//!     @value 0
//!       Don't recurse.
//!     @value 1
//!       Release the entire subtree.
//!     @value 3
//!       Release the entire subtree, but only those paths that were
//!       added automatically by a recursive monitor.
//!   @endint
//!
//! @seealso
//!   @[monitor()]
void release(string path, MonitorFlags|void flags)
{
  path = combine_path(path, ".");
  Monitor m = m_delete(monitors, path);
  if (m) {
    release_monitor(m);
  }
  if (flags && m->st && m->st->isdir) {
    path = combine_path(path, "");
    foreach(monitors; string mpath; m) {
      if (has_prefix(mpath, path) && ((m->flags & flags) == flags)) {
	m_delete(monitors, mpath);
	release_monitor(m);
      }
    }
  }
}

//! Check a single @[Monitor] for changes.
//!
//! @param m
//!   @[Monitor] to check.
//!
//! @param flags
//!   @int
//!     @value 0
//!       Don't recurse.
//!     @value 1
//!       Check all monitors for the entire subtree rooted in @[m].
//!   @endint
//!
//! This function is called by @[check()] for the @[Monitor]s
//! it considers need checking. If it detects any changes an
//! appropriate callback will be called.
//!
//! @returns
//!   Returns @expr{1@} if a change was detected and @expr{0@} (zero)
//!   otherwise.
//!
//! @note
//!   Any callbacks will be called from the same thread as the one
//!   calling @[check_monitor()].
//!
//! @note
//!   The return value can not be trusted to return @expr{1@} for all
//!   detected changes in recursive mode.
//!
//! @seealso
//!   @[check()], @[data_changed()], @[attr_changed()], @[file_created()],
//!   @[file_deleted()], @[stable_data_change()]
protected int(0..1) check_monitor(Monitor m, MonitorFlags|void flags)
{
  return m->check(flags);
}

//! Check for changes.
//!
//! @param max_wait
//!   Maximum time in seconds to wait for changes. @expr{-1}
//!   for infinite wait.
//!
//! A suitable subset of the monitored files will be checked
//! for changes.
//!
//! @returns
//!   The function returns when either a change has been detected
//!   or when @[max_wait] has expired.
//!
//! @note
//!   Any callbacks will be called from the same thread as the one
//!   calling @[check()].
//!
//! @seealso
//!   @[monitor()]
void check(int|void max_wait)
{
  while(1) {
    int cnt;
    int t = time();
    Monitor m;
    if (sizeof(monitors)) {
      while ((m = monitor_queue->peek()) &&
	     m <= t) {
	cnt += check_monitor(m);
      }
    }
    if (cnt || !max_wait) return;
    if (max_wait > 0) max_wait--;
    sleep(1);
  }
}

//! Backend to use.
//!
//! If @expr{0@} (zero) - use the default backend.
protected Pike.Backend backend;

//! Call-out identifier for @[backend_check()] if in
//! nonblocking mode.
//!
//! @seealso
//!   @[set_nonblocking()], @[set_blocking()]
protected mixed co_id;

//! Change backend.
//!
//! @param backend
//!   Backend to use. @expr{0@} (zero) for the default backend.
void set_backend(Pike.Backend|void backend)
{
  int was_nonblocking = !!co_id;
  set_blocking();
  this_program::backend = backend;
  if (was_nonblocking) {
    set_nonblocking();
  }
}

//! Turn off nonblocking mode.
//!
//! @seealso
//!   @[set_nonblocking()]
void set_blocking()
{
  if (co_id) {
    if (backend) backend->remove_call_out(co_id);
    else remove_call_out(co_id);
    co_id = 0;
  }
}

//! Backend check callback function.
//!
//! This function is intended to be called from a backend,
//! and performs a @[check()] followed by rescheduling
//! itself via a call to @[set_nonblocking()].
//!
//! @seealso
//!   @[check()], @[set_nonblocking()]
protected void backend_check()
{
  co_id = 0;
  mixed err = catch {
      check(0);
    };
  set_nonblocking();
  if (err) throw(err);
}

//! Turn on nonblocking mode.
//!
//! Register suitable callbacks with the backend to automatically
//! call @[check()].
//!
//! @[check()] and thus all the callbacks will be called from the
//! backend thread.
//!
//! @seealso
//!   @[set_blocking()], @[check()].
void set_nonblocking()
{
  if (co_id) return;
  Monitor m = monitor_queue->peek();
  int t = (m && m->next_poll - time(1)) || max_dir_check_interval;
  if (t > max_dir_check_interval) t = max_dir_check_interval;
  if (t < 0) t = 0;
  if (backend) co_id = backend->call_out(backend_check, t);
  else co_id = call_out(backend_check, t);
}

//! Set the @[default_max_dir_check_interval].
void set_max_dir_check_interval(int max_dir_check_interval)
{
  if (max_dir_check_interval > 0) {
    this_program::max_dir_check_interval = max_dir_check_interval;
  } else {
    this_program::max_dir_check_interval = default_max_dir_check_interval;
  }
}

//! Set the @[default_file_interval_factor].
void set_file_interval_factor(int file_interval_factor)
{
  if (file_interval_factor > 0) {
    this_program::file_interval_factor = file_interval_factor;
  } else {
    this_program::file_interval_factor = default_file_interval_factor;
  }
}

