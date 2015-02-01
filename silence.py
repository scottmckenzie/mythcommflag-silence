#!/usr/bin/env python
# Build a skiplist from silence in the audio track.
# v1.0 Roger Siddons
# v2.0 Fix progid for job/player messages
# v3.0 Send player messages via Python
# v3.1 Fix commflag status, pad preset. Improve style & make Python 3 compatible
# v4.0 silence.cpp will kill the head of the pipeline (tail) when recording finished
# v4.1 Use unicode for foreign chars
# v4.2 Prevent BE writeStringList errors
# v5.0 Improve exception handling/logging. Fix player messages (0.26+ only)

import MythTV
import os
import subprocess
import argparse
import collections
import re
import sys

kExe_Silence = '/usr/local/bin/silence'
kUpmix_Channels = '6' # Change this to 2 if you never have surround sound in your recordings.

class MYLOG(MythTV.MythLog):
  "A specialised logger"

  def __init__(self, db):
    "Initialise logging"
    MythTV.MythLog.__init__(self, '', db)

  def log(self, msg, level = MythTV.MythLog.INFO):
    "Log message"
    # prepend string to msg so that rsyslog routes it to mythcommflag.log logfile
    MythTV.MythLog.log(self, MythTV.MythLog.COMMFLAG, level, 'mythcommflag: ' + msg.rstrip('\n'))

class PRESET:
  "Manages the presets (parameters passed to the detection algorithm)"

  # define arg ordering and default values
  argname = ['thresh', 'minquiet', 'mindetect', 'minbreak', 'maxsep', 'pad']
  argval  = [  -75,       0.16,        6,          120,       120,    0.48]
  # dictionary holds value for each arg
  argdict = collections.OrderedDict(list(zip(argname, argval)))

  def _validate(self, k, v):
    "Converts arg input from string to float or None if invalid/not supplied"
    if v is None or v == '':
      return k, None
    try:
      return k, float(v)
    except ValueError:
      self.logger.log('Preset ' + k + ' (' + str(v) + ') is invalid - will use default',
        MYLOG.ERR)
      return k, None

  def __init__(self, _logger):
    "Initialise preset manager"
    self.logger = _logger

  def getFromArg(self, line):
    "Parses preset values from command-line string"
    self.logger.log('Parsing presets from "' + line + '"', MYLOG.DEBUG)
    if line:  # ignore empty string
      vals = [i.strip() for i in line.split(',')]  # split individual params
      # convert supplied values to float & match to appropriate arg name
      validargs = list(map(self._validate, self.argname, vals[0:len(self.argname)]))
      # remove missing/invalid values from list & replace default values with the rest
      self.argdict.update(v for v in validargs if v[1] is not None)

  def getFromFile(self, filename, title, callsign):
    "Gets preset values from a file"
    self.logger.log('Using preset file "' + filename + '"', MYLOG.DEBUG)
    try:
      with open(filename) as presets:
        for rawline in presets:
          line = rawline.strip()
          if line and (not line.startswith('#')):  # ignore empty & comment lines
            vals = [i.strip() for i in line.split(',')]  # split individual params
            # match preset name to recording title or channel
            pattern = re.compile(vals[0], re.IGNORECASE)
            if pattern.match(title) or pattern.match(callsign):
              self.logger.log('Using preset "' + line.strip() + '"')
              # convert supplied values to float & match to appropriate arg name
              validargs = list(map(self._validate, self.argname,
                         vals[1:1 + len(self.argname)]))
              # remove missing/invalid values from list &
              # replace default values with the rest
              self.argdict.update(v for v in validargs if v[1] is not None)
              break
        else:
          self.logger.log('No preset found for "' + title.encode('utf-8') + '" or "' + callsign.encode('utf-8') + '"')
    except IOError:
      self.logger.log('Presets file "' + filename + '" not found', MYLOG.ERR)
    return self.argdict

  def getValues(self):
    "Returns params as a list of strings"
    return [str(i) for i in list(self.argdict.values())]


def main():
  "Commflag a recording"
  try:
    # define options
    parser = argparse.ArgumentParser(description='Commflagger')
    parser.add_argument('--preset', help='Specify values as "Threshold, MinQuiet, MinDetect, MinLength, MaxSep, Pad"')
    parser.add_argument('--presetfile', help='Specify file containing preset values')
    parser.add_argument('--chanid', type=int, help='Use chanid for manual operation')
    parser.add_argument('--starttime', help='Use starttime for manual operation')
    parser.add_argument('--dump', action="store_true", help='Generate stack trace of exception')
    parser.add_argument('jobid', nargs='?', help='Myth job id')

    # must set up log attributes before Db locks them
    MYLOG.loadArgParse(parser)
    MYLOG._setmask(MYLOG.COMMFLAG)

    # parse options
    args = parser.parse_args()

    # connect to backend
    db = MythTV.MythDB()
    logger = MYLOG(db)
    be = MythTV.BECache(db=db)

    logger.log('')	# separate jobs in logfile
    if args.jobid:
      logger.log('Starting job %s'%args.jobid, MYLOG.INFO)
      job = MythTV.Job(args.jobid, db)
      chanid = job.chanid
      starttime = job.starttime
    elif args.chanid and args.starttime:
      job = None
      chanid = args.chanid
      try:
        # only 0.26+
        starttime = MythTV.datetime.duck(args.starttime)
      except AttributeError:
        starttime = args.starttimeaction="store_true"
    else:
      logger.log('Both --chanid and -starttime must be specified', MYLOG.ERR)
      sys.exit(1)

    # mythplayer update message uses a 'chanid_utcTimeAsISODate' format to identify recording
    try:
      # only 0.26+
      utc = starttime.asnaiveutc()
    except AttributeError:
      utc = starttime

    progId = '%d_%s'%(chanid, str(utc).replace(' ', 'T'))

    # get recording
    logger.log('Seeking chanid %s, starttime %s' %(chanid, starttime), MYLOG.INFO)
    rec = MythTV.Recorded((chanid, starttime), db)
    channel = MythTV.Channel(chanid, db)

    logger.log('Processing: ' + channel.callsign.encode('utf-8') + ', ' + str(rec.starttime)
      + ', "' + rec.title.encode('utf-8') + ' - ' + rec.subtitle.encode('utf-8')+ '"')

    sg = MythTV.findfile(rec.basename, rec.storagegroup, db)
    if sg is None:
      logger.log("Can't access file %s from %s"%(rec.basename, rec.storagegroup), MYLOG.ERR)
      try:
        job.update({'status': job.ERRORED, 'comment': "Couldn't access file"})
      except AttributeError : pass
      sys.exit(1)

    # create params with default values
    param = PRESET(logger)
    # read any supplied presets
    if args.preset:
      param.getFromArg(args.preset)
    elif args.presetfile:  # use preset file
      param.getFromFile(args.presetfile, rec.title, channel.callsign)

    # Pipe file through ffmpeg to extract uncompressed audio stream. Keep going till recording is finished.
    infile = os.path.join(sg.dirname, rec.basename)
    p1 = subprocess.Popen(["tail", "--follow", "--bytes=+1", infile], stdout=subprocess.PIPE)
    p2 = subprocess.Popen(["mythffmpeg", "-loglevel", "quiet", "-i", "pipe:0",
                "-f", "au", "-ac", kUpmix_Channels, "-"],
                stdin=p1.stdout, stdout=subprocess.PIPE)
    # Pipe audio stream to C++ silence which will spit out formatted log lines
    p3 = subprocess.Popen([kExe_Silence, "%d" % p1.pid] + param.getValues(), stdin=p2.stdout,
                stdout=subprocess.PIPE)

    # Purge any existing skip list and flag as in-progress
    rec.commflagged = 2
    rec.markup.clean()
    rec.update()

    # Process log output from C++ silence
    breaks = 0
    level = {'info': MYLOG.INFO, 'debug': MYLOG.DEBUG, 'err': MYLOG.ERR}
    while True:
      line = p3.stdout.readline()
      if line:
        flag, info = line.split('@', 1)
        if flag == 'cut':
          # extract numbers from log line
          numbers = re.findall('\d+', info)
          logger.log(info)
          # mark advert in database
          rec.markup.append(int(numbers[0]), rec.markup.MARK_COMM_START, None)
          rec.markup.append(int(numbers[1]), rec.markup.MARK_COMM_END, None)
          rec.update()
          breaks += 1
          # send new advert skiplist to MythPlayers
          skiplist = ['%d:%d,%d:%d'%(x, rec.markup.MARK_COMM_START, y, rec.markup.MARK_COMM_END)
                   for x, y in rec.markup.getskiplist()]
          mesg = 'COMMFLAG_UPDATE %s %s'%(progId, ','.join(skiplist))
#         logger.log('  Sending %s'%mesg,  MYLOG.DEBUG)
          result = be.backendCommand("MESSAGE[]:[]" + mesg)
          if result != 'OK':
            logger.log('Sending update message to backend failed, response = %s, message = %s'% (result, mesg), MYLOG.ERR)
        elif flag in level:
          logger.log(info, level.get(flag))
        else:  # unexpected prefix
          # use warning for unexpected log levels
          logger.log(flag, MYLOG.WARNING)
      else:
        break

    # Signal comflagging has finished
    rec.commflagged = 1
    rec.update()

    logger.log('Detected %s adverts.' % breaks)
    try:
      job.update({'status': 272, 'comment': 'Detected %s adverts.' % breaks})
    except AttributeError : pass

    # Finishing too quickly can cause writeStringList/socket errors in the BE. (pre-0.28 only?)
    # A short delay prevents this
    import time
    time.sleep(1)

  except Exception as e:
    # get exception before we generate another
    import traceback
    exc_type, exc_value, frame = sys.exc_info()
    # get stacktrace as a list
    stack = traceback.format_exception(exc_type, exc_value, frame)

    # set status
    status = 'Failed due to: "%s"'%e
    try:
      logger.log(status, MYLOG.ERR)
    except : pass
    try:
      job.update({'status': job.ERRORED, 'comment': 'Failed.'})
    except : pass

    # populate stack trace with vars
    try:
      if args.dump:
        # insert the frame local vars after each frame trace
        # i is the line index following the frame trace; 0 is the trace mesg, 1 is the first code line
        i = 2
        while frame is not None:
          # format local vars
          vars = []
          for name, var in frame.tb_frame.f_locals.iteritems():
            try:
              text = '%s' % var
              # truncate vars that are too long
              if len(text) > 1000:
                text = text[:1000] + '...'

            except Exception as e: # some var defs may be incomplete
              text = '<Unknown due to: %s>'%e
            vars.append('@ %s = %s'%(name, text))
          # insert local stack contents after code trace line
          stack.insert(i, '\n'.join(['@-------------'] + vars + ['@-------------\n']))
          # advance over our insertion & the next code trace line
          i += 2
          frame = frame.tb_next
        logger.log('\n'.join(stack), MYLOG.ERR)
    except : pass
    sys.exit(1)

if __name__ == '__main__':
  main()
