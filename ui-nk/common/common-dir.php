<?php
/***********************************************************
 Copyright (C) 2008 Hewlett-Packard Development Company, L.P.

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 version 2 as published by the Free Software Foundation.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
***********************************************************/

/*************************************************
 Restrict usage: Every PHP file should have this
 at the very beginning.
 This prevents hacking attempts.
 *************************************************/
global $GlobalReady;
if (!isset($GlobalReady)) { exit; }

function Isdir($mode) { return(($mode & 1<<18) + ($mode & 0040000) != 0); }
function Isartifact($mode) { return(($mode & 1<<28) != 0); }
function Iscontainer($mode) { return(($mode & 1<<29) != 0); }

/************************************************************
 DirMode2String(): Convert a mode to string values.
 ************************************************************/
function DirMode2String($Mode)
{
  $V="";
  if (Isartifact($Mode)) { $V .= "a"; } else { $V .= "-"; }
  if (($Mode & 0120000) == 0120000) { $V .= "l"; } else { $V .= "-"; }
  if (Isdir($Mode)) { $V .= "d"; } else { $V .= "-"; }

  if ($Mode & 0000400) { $V .= "r"; } else { $V .= "-"; }
  if ($Mode & 0000200) { $V .= "w"; } else { $V .= "-"; }
  if ($Mode & 0000100)
    {
    if ($Mode & 0004000) { $V .= "s"; } /* setuid */
    else { $V .= "x"; }
    }
  else
    {
    if ($Mode & 0004000) { $V .= "S"; } /* setuid */
    else { $V .= "-"; }
    }

  if ($Mode & 0000040) { $V .= "r"; } else { $V .= "-"; }
  if ($Mode & 0000020) { $V .= "w"; } else { $V .= "-"; }
  if ($Mode & 0000010)
    {
    if ($Mode & 0002000) { $V .= "s"; } /* setgid */
    else { $V .= "x"; }
    }
  else
    {
    if ($Mode & 0002000) { $V .= "S"; } /* setgid */
    else { $V .= "-"; }
    }

  if ($Mode & 0000004) { $V .= "r"; } else { $V .= "-"; }
  if ($Mode & 0000002) { $V .= "w"; } else { $V .= "-"; }
  if ($Mode & 0000001)
    {
    if ($Mode & 0001000) { $V .= "t"; } /* sticky bit */
    else { $V .= "x"; }
    }
  else
    {
    if ($Mode & 0001000) { $V .= "T"; } /* setgid */
    else { $V .= "-"; }
    }

  return($V);
} // DirMode2String()

/************************************************************
 DirGetNonArtifact(): Given an artifact directory (uploadtree_pk),
 return the first non-artifact directory (uploadtree_pk).
 TBD: "username" will be added in the future and it may change
 how this function works.
 NOTE: This is recursive!
 ************************************************************/
$DirGetNonArtifact_Prepared=0;
function DirGetNonArtifact($UploadtreePk)
{
  global $Plugins;
  global $DB;
  if (empty($DB)) { return; }

  /* Get contents of this directory */
  global $DirGetNonArtifact_Prepared;
  if (!$DirGetNonArtifact_Prepared)
    {
    $DirGetNonArtifact_Prepared=1;
    $DB->Prepare("DirGetNonArtifact",'SELECT * FROM uploadtree LEFT JOIN ufile ON ufile.ufile_pk = uploadtree.ufile_fk LEFT JOIN pfile ON pfile.pfile_pk = ufile.pfile_fk WHERE parent = $1;');
    }
  $Children = $DB->Execute("DirGetNonArtifact",array($UploadtreePk));
  $Recurse=NULL;
  foreach($Children as $C)
    {
    if (empty($C['ufile_mode'])) { continue; }
    if (!Isartifact($C['ufile_mode']))
	{
	return($UploadtreePk);
	}
    if (($C['ufile_name'] == 'artifact.dir') ||
        ($C['ufile_name'] == 'artifact.unpacked'))
	{
	$Recurse = DirGetNonArtifact($C['uploadtree_pk']);
	}
    }
  if (!empty($Recurse))
    {
    return(DirGetNonArtifact($Recurse));
    }
  return($UploadtreePk);
} // DirGetNonArtifact()

/************************************************************
 _DirCmp(): Compare function for usort() on directory items.
 ************************************************************/
function _DirCmp($a,$b)
{
  return(strcmp($a['ufile_name'],$b['ufile_name']));
}

/************************************************************
 DirGetList(): Given a directory (uploadtree_pk),
 return the directory contents.
 TBD: "username" will be added in the future and it may change
 how this function works.
 Returns array containing:
   uploadtree_pk,ufile_pk,pfile_pk,ufile_name,ufile_mode
 ************************************************************/
$DirGetList_Prepared=0;
function DirGetList($Upload,$UploadtreePk)
{
  global $Plugins;
  global $DB;
  if (empty($DB)) { return; }

  /* Get the basic directory contents */
  global $DirGetList_Prepared;
  if (!$DirGetList_Prepared)
    {
    $DirGetList_Prepared=1;
    $DB->Prepare("DirGetList_1",'SELECT * FROM uploadtree LEFT JOIN ufile ON ufile.ufile_pk = uploadtree.ufile_fk LEFT JOIN pfile ON pfile.pfile_pk = ufile.pfile_fk WHERE upload_fk = $1 AND uploadtree.parent IS NULL ORDER BY ufile.ufile_name ASC;');
    $DB->Prepare("DirGetList_2",'SELECT * FROM uploadtree LEFT JOIN ufile ON ufile.ufile_pk = uploadtree.ufile_fk LEFT JOIN pfile ON pfile.pfile_pk = ufile.pfile_fk WHERE upload_fk = $1 AND uploadtree.parent = $2 ORDER BY ufile.ufile_name ASC;');
    }
  if (empty($UploadtreePk)) { $Results=$DB->Execute("DirGetList_1",array($Upload)); }
  else { $Results=$DB->Execute("DirGetList_2",array($Upload,$UploadtreePk)); }
  usort($Results,_DirCmp);

  /* Replace all artifact directories */
  foreach($Results as $Key => $Val)
    {
    /* if artifact and directory */
    $R = &$Results[$Key];
    if (Isartifact($R['ufile_mode']) && Isdir($R['ufile_mode']))
	{
	$R['uploadtree_pk'] = DirGetNonArtifact($R['uploadtree_pk']);
	}
    }
  return($Results);
} // DirGetList()

/************************************************************
 Dir2Path(): given an uploadtree_pk, return an array containing
 the path.  Each element in the path is an array containing
 ufile and uploadtree information.
 ************************************************************/
function Dir2Path($UploadtreePk, $UfilePk=-1)
{
  global $Plugins;
  global $DB;
  if (empty($DB)) { return; }

  $Path = array();	// return path array

  // build the array from the given node to the top and then reverse it so
  // it goes top down before returning it.

  /* Add the ufile first (if it exists) */
  if ($UfilePk >= 0)
    {
    $Sql = "SELECT * FROM uploadtree LEFT JOIN ufile ON uploadtree.ufile_fk=ufile.ufile_pk WHERE ufile_pk = $UfilePk LIMIT 1;";
    $Results = $DB->Action($Sql);
    $Row = $Results[0];
    array_push($Path,$Row);
    }

  while(!empty($UploadtreePk))
    {
    $Sql = "SELECT * FROM uploadtree LEFT JOIN ufile ON uploadtree.ufile_fk=ufile.ufile_pk WHERE uploadtree_pk = $UploadtreePk LIMIT 1;";
    $Results = $DB->Action($Sql);
    $Row = $Results[0];
    if (!empty($Row['ufile_name']) && !Isartifact($Row['ufile_mode']) && ($UfilePk != $Row['ufile_pk']))
	{
	$Row['uploadtree_pk'] = DirGetNonArtifact($Row['uploadtree_pk']);
	array_push($Path,$Row);
	}
    $UploadtreePk = $Row['parent'];
    }
  return(array_reverse($Path,false));
} // Dir2Path()

/************************************************************
 Dir2Browse(): given an uploadtree_pk and ufile_pk, return a
 string listing the browse paths.
 ************************************************************/
function Dir2Browse ($Mod, $UploadtreePk, $UfilePk, $LinkLast=NULL,
		     $ShowBox=1, $ShowMicro=NULL, $Enumerate=-1)
{
  global $Plugins;
  global $DB;

  $V = "";
  if ($ShowBox)
    {
    $V .= "<div style='border: thin dotted gray; background-color:lightyellow'>\n";
    }

  if ($Enumerate >= 0)
    {
    $V .= "<font size='+2'>" . number_format($Enumerate,0,"",",") . ":</font>";
    }

  $Opt = Traceback_parm_keep(array("folder","show"));
  $Uri = Traceback_uri() . "?mod=$Mod";

  $Path = Dir2Path($UploadtreePk,$UfilePk);
  $Last = &$Path[count($Path)-1];

  $V .= "<font class='text'>\n<b>Folder</b>: ";
  $List = FolderGetFromUpload($Path[0]['upload_fk']);
  $Uri2 = Traceback_uri() . "?mod=browse" . Traceback_parm_keep(array("show"));
  for($i=0; $i < count($List); $i++)
    {
    $Folder = $List[$i]['folder_pk'];
    $FolderName = htmlentities($List[$i]['folder_name']);
    $V .= "<b><a href='$Uri2&folder=$Folder'>$FolderName</a></b>/ ";
    }
  $FirstPath=1;
  foreach($Path as $P)
    {
    if (empty($P['ufile_name'])) { continue; }
    if (!$FirstPath) { $V .= "/ "; }
    if (!empty($LinkLast) || ($P != $Last))
	{
	if ($P == $Last)
	  {
	  $Uri = Traceback_uri() . "?mod=$LinkLast";
	  if (!empty($P['ufile_fk'])) { $Uri .= "&ufile=" . $P['ufile_fk']; }
	  if (!empty($P['pfile_fk'])) { $Uri .= "&pfile=" . $P['pfile_fk']; }
	  }
	$V .= "<a href='$Uri&upload=" . $P['upload_fk'] . "&item=" . $P['uploadtree_pk'] . $Opt . "'>";
	}
    if (Isdir($P['ufile_mode']))
	{
	$V .= $P['ufile_name'];
	}
    else
	{
	if ($P != $Last) { $V .= "<br>\n&nbsp;&nbsp;"; }
	$V .= "<b>" . $P['ufile_name'] . "</b>";
	}
    if (!empty($LinkLast) || ($P != $Last))
	{
	$V .= "</a>";
	}
    $FirstPath=0;
    }
  $V .= "</font>\n";

  if (!empty($ShowMicro))
    {
    $MenuDepth = 0; /* unused: depth of micro menu */
    $V .= menu_to_1html(menu_find($ShowMicro,$MenuDepth),1);
    }

  if ($ShowBox)
    {
    $V .= "</div>\n";
    }
  return($V);
} // Dir2Browse()

/************************************************************
 Dir2BrowseUpload(): given an upload_pk, return a string listing
 the browse paths.
 This calls Dir2Browse().
 ************************************************************/
function Dir2BrowseUpload ($Mod, $UploadPk, $UfilePk, $LinkLast=NULL, $ShowBox=1, $ShowMicro=NULL)
{
  global $DB;
  /* Find the file associated with the upload */
  $SQL = "SELECT uploadtree_pk FROM upload INNER JOIN uploadtree ON upload_fk = '$UploadPk' AND upload.ufile_fk = uploadtree.ufile_fk;";
  $Results = $DB->Action($SQL);
  $UploadtreePk = $Results[0]['uploadtree_pk'];
  return(Dir2Browse($Mod,$UploadtreePk,$UfilePk,$LinkLast,$ShowBox,$ShowMicro));
} // Dir2BrowseUpload()

?>
