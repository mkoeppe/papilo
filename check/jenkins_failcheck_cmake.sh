#!/bin/bash -x
#* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
#*                                                                           *
#*                  This file is part of the program and library             *
#*         SCIP --- Solving Constraint Integer Programs                      *
#*                                                                           *
#*    Copyright (C) 2002-2020 Konrad-Zuse-Zentrum                            *
#*                            fuer Informationstechnik Berlin                *
#*                                                                           *
#*  SCIP is distributed under the terms of the ZIB Academic License.         *
#*                                                                           *
#*  You should have received a copy of the ZIB Academic License              *
#*  along with SCIP; see the file COPYING. If not email to scip@zib.de.      *
#*                                                                           *
#* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *

# This script uploads and checks for fails in a SCIP run.
# Sends an email if errors are detected. Is not meant to be use directly,
# but to be called by jenkins_check_results_cmake.sh.
# Note: TESTSET, GITHASH, etc are read from the environment, see
# jenkins_check_results_cmake.sh

sleep 5

echo "This is jenkins_failcheck_cmake.sh running."

#################################
# AWK scripts for later         #
#################################
read -d '' awkscript_findasserts <<'EOF'
# call: awk "$awkscript_findasserts" $ERRORINSTANCES $ERRFILE (or pipe a string with the error instances into awk if they are not in a file)

# init variables
BEGIN {
    searchAssert=0;
 }

# read failed instances into array as keys
# the names in the errorinstances have to be the full names of the instances
# set failed[instname] to
#     1 if instance found in err file once, and no assert found
#     2 if instance found in err file once, also found assert
#     3 if instance found more than once in errfile
NR==FNR && /fail.*abort/ {
    failed[$1]=0; next;
 }

# find instances in errorfile
NR!=FNR && /^@01/ {

    # if we were looking for an assertion in currinstname, we are now at the beginning of the error output
    # of another instance. Therefore, we didn't find an assertion and this instance needs human inspection.
    if (searchAssert == 1) { searchAssert=0; }

    # get instancename (copied from check.awk)
    n = split($2, a, "/"); m = split(a[n], b, "."); currinstname = b[1];
    if( b[m] == "gz" || b[m] == "z" || b[m] == "GZ" || b[m] == "Z" ) { m--; }
    for( i = 2; i < m; ++i ) { currinstname = currinstname "." b[i]; }

    instancestr = $2;

    # adjust searchAssert
    if (currinstname in failed) {
        searchAssert=1;
        if ( failed[currinstname] == 0 ) {
            failed[currinstname]=1
         } else {
            failed[currinstname]=3
         }
      }
   }

# find assertions in errorfile
NR!=FNR && searchAssert == 1 && /Assertion.*failed.$/ {
    print "";
    print instancestr
    for(i=2;i<=NF;i++){printf "%s ", $i}; print "";
    if ( failed[currinstname] < 3 ) {
        failed[currinstname]=2
     } else {
        failed[currinstname]=3
     }
    searchAssert=0;
 }

# print results
END {
    print "";
    count = 0;
    for( key in failed ) {
        if( failed[key] == 1 ) {
            if( count == 0 ) {
                print "The following fails need human inspection, because there was no Assertion found:";
             }
            print key;
            count = count + 1;
         }
      }
    count = 0;
    for( key in failed ) {
        if( failed[key] == 3 ) {
            if( count == 0 ) {
                print "The following instances do not appear in the .err file, or some other error appeared:";
             }
            print key;
            count = count + 1;
         }
      }
   }
EOF

read -d '' awkscript_checkfixedinstances <<'EOF'
# call: awk $AWKARGS "$awkscript_checkfixedinstances" $RESFILE $DATABASE
# prints fixed instances
# afterwards $TMPDATABASE contains all still failing bugs, not new ones!

# read fail instances for this configuration from resfile
NR == FNR && /fail/ {
    failmsg=$13; for(i=14;i<=NF;i++){ failmsg=failmsg"_"$i; }
    errorstring=$1 " " failmsg " " GITBRANCH " " TESTSET " " SETTINGS " " SCIP_BUILDDIR " " PERM;
    bugs[errorstring]
    next;
 }

# read from database
NR != FNR {
    if( $3 == GITBRANCH && $4 == TESTSET && $5 == SETTINGS && $6 == SCIP_BUILDDIR ) {
        if (!( $0 in bugs )) {
            # if current line from database matches our settings and
            # it is not in the set of failed instances from this run it was fixed
            print "Previously failing instance " $1 " with error " $2 " does not fail anymore"
            next;
         }
      }
    # write record into the database for next time
    print $0 >> TMPDATABASE
 }
EOF

read -d '' awkscript_readknownbugs <<'EOF'
# call: awk $AWKARGS "$awkscript_readknownbugs" $DATABASE $RESFILE
# append new fails to DATABASE, also print them
# write all still failing bugs into STILLFAILING

# read known bugs from database
NR == FNR {known_bugs[$0]; next}

# find fails in resfile
/fail/ {
    # get the fail error and build string in database format
    failmsg=$13; for(i=14;i<=NF;i++){failmsg=failmsg"_"$i;}
    errorstring=$1 " " failmsg " " GITBRANCH " " TESTSET " " SETTINGS " " SCIP_BUILDDIR " " PERM;

    if (!( errorstring in known_bugs )) {
        # if error is not known, add it and print it to ERRORINSTANCES for sending mail later
        print errorstring >> DATABASE;
        print $0;
     } else {
        # if error is known, then instance failed before with same settings
        # only report the name and the fail message of the instance
        print $1 " " failmsg >> STILLFAILING;
     }
  }
EOF

read -d '' awkscript_scipheader <<'EOF'
# call: awk "$awkscript_scipheader" $OUTFILE
# prints current scipheader from OURFILE

BEGIN{printLines=0;}

/^SCIP version/ {printLines=1;}
printLines > 0 && /^$/ {printLines+=1;}

# UG does not have blank lines after the header.
printLines > 0 && /^Default LC presolving/ {exit 0;}

# the third blank line marks the end of the header
printLines > 0 {print $0}
{
    if ( printLines == 3 ){
        exit 0;
     }
  }
EOF
#################################
# End of AWK Scripts            #
#################################

# checks if branch has something pending
function parse_git_dirty() {
  git diff --quiet --ignore-submodules HEAD 2>/dev/null; [ $? -eq 1 ] && echo "*"
}

# gets the current git branch
function parse_git_branch() {
  git branch --no-color 2> /dev/null | sed -e '/^[^*]/d' -e "s/* \(.*\)/\1$(parse_git_dirty)/"
}

GITBRANCH="$(parse_git_branch)"

: ${RUBBERBAND_UPLOAD:=no}
: ${CHECKFAILS:=no}

echo "----------- env ----------"
env | sort
echo "----------- end env ----------"

if [ "${PERFORMANCE}" == "performance" ]; then
  RUBBERBAND_UPLOAD=yes
  CHECKFAILS=yes
  # MODE is performance
elif [ "${PERFORMANCE}" == "debug" ]; then
  RUBBERBAND_UPLOAD=no
  CHECKFAILS=yes
  # MODE is debug
elif [ "${PERFORMANCE}" == "mergerequest" ]; then
  RUBBERBAND_UPLOAD=yes
  CHECKFAILS=no
  # MODE is performance
elif [ "${PERFORMANCE}" == "valgrind" ]; then
  RUBBERBAND_UPLOAD=no
  CHECKFAILS=yes
  MODE=relwithdebinfo
fi

# EXECUTABLE has form 'scipoptspx_bugfix_20180401/bin/scip', we only want 'scipoptspx'
SCIP_BUILDDIR=$(echo ${EXECUTABLE} | cut -d '/' -f 1 | cut -d '_' -f 1)
SOPLEX_HASH=$(${PWD}/${EXECUTABLE} -v | grep "  SoPlex" | grep "GitHash: .*]" -o | cut -d ' ' -f 2 | cut -d ']' -f 1)

NEWTIMESTAMP=$(date '+%F-%H-%M')
# The RBDB database has the form: timestamp_of_testrun rubberbandid p=PERM s=SEED
if [ "${PERFORMANCE}" == "performance" ]; then
  RBDB="/nfs/OPTI/adm_timo/databases/rbdb/${GITBRANCH}_${MODE}_${TESTSET}_${SETTINGS}_${SCIP_BUILDDIR}_rbdb.txt"
  touch $RBDB
  OLDTIMESTAMP=$(tail -n 1 ${RBDB} | cut -d ' ' -f 1)
elif [ "${PERFORMANCE}" == "mergerequest" ]; then
  RBDB="${PWD}/${MODE}_mergerequest_${TESTSET}_${gitlabMergeRequestIid}_rbdb.txt"
  touch $RBDB
fi

if [ "$PSMESSAGE" != "" ]; then
  PSMESSAGE="
   PS: $PSMESSAGE"
fi

: ${GLBSEEDSHIFT:=0}
: ${STARTPERM:=0}

SEEDSBND=$(expr ${SEEDS} + ${GLBSEEDSHIFT})
PERMUTEBND=$(expr ${PERMUTE} + ${STARTPERM})

SEED=${GLBSEEDSHIFT}

while [ "${SEED}" -le "${SEEDSBND}" ]; do
  # get ending given by seed
  if [ "${SEED}" == "0" ]; then
    SEED_ENDING=""
  else
    SEED_ENDING="-s${SEED}"
  fi
  PERM=${STARTPERM}
  while [ "${PERM}" -le "${PERMUTEBND}" ]; do
    # get ending given by permutation
    if [ "${PERM}" == "0" ]; then
      PERM_ENDING=""
    else
      PERM_ENDING="-p${PERM}"
    fi

    # we use a name that is unique per test sent to the cluster (a jenkins job
    # can have several tests sent to the cluster, that is why the jenkins job
    # name (i.e, the directory name) is not enough)
    if [ "${PERFORMANCE}" == "mergerequest" ]; then
      DATABASE="${PWD}/${GITBRANCH}_${MODE}_${TESTSET}_${SETTINGS}_${SCIP_BUILDDIR}${SEED_ENDING}${PERM_ENDING}.txt"
    elif [ "${PERFORMANCE}" != "mergerequest" ]; then
      DATABASE="/nfs/OPTI/adm_timo/databases/${IDENT}${GITBRANCH}_${MODE}_${TESTSET}_${SETTINGS}_${SCIP_BUILDDIR}${SEED_ENDING}${PERM_ENDING}.txt"
    fi
    touch ${DATABASE}
    TMPDATABASE="${DATABASE}.tmp"
    STILLFAILING="${DATABASE}_SF.tmp"
    OUTPUT="${DATABASE}_output.tmp"
    touch ${STILLFAILING}

    SUBJECTINFO="[BRANCH: $GITBRANCH] [TESTSET: $TESTSET] [SETTINGS: $SETTINGS] [GITHASH: $GITHASH] "

    AWKARGS="-v GITBRANCH=$GITBRANCH -v TESTSET=$TESTSET -v SETTINGS=$SETTINGS -v SCIP_BUILDDIR=$SCIP_BUILDDIR -v DATABASE=$DATABASE -v TMPDATABASE=$TMPDATABASE -v STILLFAILING=$STILLFAILING -v PERM=$PERM -v SEED=$SEED -v MODE=$MODE"
    echo $AWKARGS

    # the first time, the file might not exists so we create it
    # Even more, we have to write something to it, since otherwise
    # the awk scripts below won't work (NR and FNR will not be different)
    if ! [[ -s $DATABASE ]]; then # check that file exists and has size larger that 0
      echo "Preparing database."
      echo "Instance Fail_reason Branch Testset Settings Opt_mode SCIP_BUILDDIR" >$DATABASE
    fi

    EMAILFROM="adm_timo <timo-admin@zib.de>"
    EMAILTO="<timo-admin@zib.de>"

    if [ "${GIT_AUTHOR_EMAIL}" != "" ]; then
      EMAILTO="${GIT_AUTHOR_EMAIL}"
    fi

    #################
    # FIND evalfile #
    #################

    # SCIP check files are in check/${OUTPUTDIR}
    BASEFILE="check/${OUTPUTDIR}/check.${TESTSET}.*.${SETTINGS}${SEED_ENDING}${PERM_ENDING}."
    EVALFILE=$(ls ${BASEFILE}*eval)

    # at this point we have exactly one evalfile
    BASENAME=${EVALFILE%.*} # remove extension
    WORKINGDIR=$(pwd -P)

    # Store paths of err out res and set file
    ERRFILE="${WORKINGDIR}/${BASENAME}.err"
    OUTFILE="${WORKINGDIR}/${BASENAME}.out"
    RESFILE="${WORKINGDIR}/${BASENAME}.res"
    SETFILE="${WORKINGDIR}/${BASENAME}.set"
    EVALFILE="${WORKINGDIR}/${BASENAME}.eval"

    ############################################
    # Process evalfile and upload to ruberband #
    ############################################

    # evaluate the run and upload it to rubberband
    echo "Evaluating the run and uploading it to rubberband."
    cd check/
    PERF_MAIL=""
    if [ "${RUBBERBAND_UPLOAD}" == "yes" ]; then
      # add tags to uploaded run
      export RBCLI_TAG="${GITBRANCH},${PERFORMANCE}"
      if [ "${MODE}" == "debug" ]; then
        ./evalcheck_cluster.sh -E ${EVALFILE} >${OUTPUT}
      else
        ./evalcheck_cluster.sh -U ${EVALFILE} >${OUTPUT}
      fi
      NEWRBID=$(cat $OUTPUT | grep "rubberband.zib" | sed -e 's|https://rubberband.zib.de/result/||')
      if [ "${SOPLEX_HASH}" != "" ]; then
        echo "${NEWTIMESTAMP} ${NEWRBID} p=${PERM} s=${SEED} fullgh=${FULLGITHASH} soplexhash=${SOPLEX_HASH}" >>$RBDB
      else
        echo "${NEWTIMESTAMP} ${NEWRBID} p=${PERM} s=${SEED} fullgh=${FULLGITHASH}" >>$RBDB
      fi
    else
      ./evalcheck_cluster.sh "-v useshortnames=0" ${EVALFILE} >${OUTPUT}
    fi
    cat ${OUTPUT}
    rm ${OUTPUT}
    cd ..

    if [ "${CHECKFAILS}" == "yes" ]; then
      # check for fixed instances
      echo "Checking for fixed instances."
      RESOLVEDINSTANCES=$(awk $AWKARGS "$awkscript_checkfixedinstances" $RESFILE $DATABASE)
      echo "Temporary database: $TMPDATABASE\n"
      mv $TMPDATABASE $DATABASE

      if [ "${PERFORMANCE}" == "debug" ]; then
        ###################
        # Check for fails #
        ###################

        # if there are fails; process them and send email when there are new ones
        NFAILS=$(grep -c fail $RESFILE)
        if [ $NFAILS -gt 0 ]; then
          echo "Detected ${NFAILS} fails."
          ## read all known bugs
          ERRORINSTANCES=$(awk $AWKARGS "$awkscript_readknownbugs" $DATABASE $RESFILE)
          STILLFAILINGDB=$(cat ${STILLFAILING})

          # check if there are new fails!
          if [ -n "$ERRORINSTANCES" ]; then
            ###################
            ## Process fails ##
            ###################

            # get SCIP's header
            SCIP_HEADER=$(awk "$awkscript_scipheader" $OUTFILE)

            # Get assertions and instance where they were generated
            ERRORS_INFO=$(echo "${ERRORINSTANCES}" | awk "$awkscript_findasserts" - ${ERRFILE})

            ###############
            # ERROR EMAIL #
            ###############
            echo "Found new errors, sending emails."
            SUBJECT="FAIL ${SUBJECTINFO}"
            echo -e "There are newly failed instances.
                  The instances run with the following SCIP version and setting file:

                  \`\`\`
                  BRANCH: $GITBRANCH

                  SCIP HEADER:
                  ${SCIP_HEADER}

                  SETTINGS FILE:
                  ${SETFILE}

                  BINARY:
                  ${PWD}/${EXECUTABLE}
                  \`\`\`

                  Here is a list of the instances and the assertion that fails (fails with _fail (abort)_), if any:
                  ${ERRORS_INFO}

                  Here is the complete list of new fails:
                  ${ERRORINSTANCES}

                  The following instances are still failing:
                  ${STILLFAILINGDB}

                  Finally, the err, out and res file can be found here:
                  $ERRFILE
                  $OUTFILE
                  $RESFILE

                  Please note that they might be deleted soon
                  ${PSMESSAGE}" | mailx -s "$SUBJECT" -r "$EMAILFROM" $EMAILTO
          else
            echo "No new errors, sending no emails."
          fi
        else
          echo "No fails detected."
        fi
      fi

      # send email if there are fixed instances
      if [ -n "$RESOLVEDINSTANCES" ]; then
        #########################
        # RESOLVED ERRORS EMAIL #
        #########################
        SUBJECT="FIX ${SUBJECTINFO}"
        echo -e "Congratulations, see bottom for fixed instances!

            The following instances are still failing:
            ${STILLFAILINGDB}

            The err, out and res file can be found here:
            $ERRFILE
            $OUTFILE
            $RESFILE

            The following errors have been fixed:
            ${RESOLVEDINSTANCES}" | mailx -s "$SUBJECT" -r "$EMAILFROM" $EMAILTO
      fi
      rm ${STILLFAILING}
    fi

    PERM=$((PERM + 1))
  done
  SEED=$((SEED + 1))
done

function geturl() {
  RBDB_STRS="$1"
  i=0
  while read -r line; do
    arr=($line)
    RBIDS[$i]=${arr[-1]}
    ((i++))
  done <<<"${RBDB_STRS}"

  IDSTR=$(printf ",%s" "${RBIDS[@]}")
  IDSTR=${IDSTR:1}

  URLSTR=$(echo ${IDSTR} | sed 's/,/?compare=/')

  echo ${URLSTR}
}

# collect all ids with timestamps OLDTIMESTAMP NEWTIMESTAMP in RBIDS
RBDB_STRS=$(grep -e "\(${OLDTIMESTAMP}\|${NEWTIMESTAMP}\)" ${RBDB} | cut -d ' ' -f 2)

URLSTR=$(geturl "${RBDB_STRS}")

PERF_MAIL=$(echo "The results of the papilo performance run are ready. Take a look at https://rubberband.zib.de/result/${URLSTR}")


SUBJECT="PAPILO PERF_MAIL ${SUBJECTINFO}"
echo -e "$PERF_MAIL" | mailx -s "$SUBJECT" -r "$EMAILFROM" $EMAILTO
echo -e "$PERF_MAIL" | mailx -s "$SUBJECT" -r "$EMAILFROM" "hoen@zib.de"
echo -e "$PERF_MAIL" | mailx -s "$SUBJECT" -r "$EMAILFROM" "$GIT_AUTHOR_EMAIL"