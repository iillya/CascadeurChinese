{ Only installer-owned snapshot filenames are deleted. Never recurse through
  ChineseLauncher or unknown directories. Called after restoration succeeds. }
procedure DeleteOwnedFile(FileName: String);
begin
  if FileExists(FileName) and not DeleteFile(FileName) then
    RaiseException('无法清理补丁文件，请关闭占用程序后重试：' + FileName);
end;

function IsSnapshotName(Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  if (Length(Name) < 15) or (Length(Name) > 30) then Exit;
  if Name[9] <> '-' then Exit;
  for I := 1 to 15 do
    if (I <> 9) and ((Name[I] < '0') or (Name[I] > '9')) then Exit;
  if Length(Name) > 15 then begin
    if (Length(Name) < 17) or (Name[16] <> '-') then Exit;
    for I := 17 to Length(Name) do
      if (Name[I] < '0') or (Name[I] > '9') then Exit;
  end;
  Result := True;
end;

procedure CleanSnapshot(Directory: String);
var
  I: Integer;
begin
  for I := 0 to GetArrayLength(PayloadNames) - 1 do
    DeleteOwnedFile(Directory + '\' + PayloadNames[I]);
  DeleteOwnedFile(Directory + '\install-state.ini');
  RemoveDir(Directory + '\translations');
  RemoveDir(Directory);
end;

procedure CleanupInstallerBackups;
var
  Entry: TFindRec;
  Directory: String;
begin
  Directory := InstallRoot + '\.inno\backups';
  if FindFirst(Directory + '\*', Entry) then begin
    try
      repeat
        if ((Entry.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) and
          ((Entry.Attributes and $400) = 0) and IsSnapshotName(Entry.Name) then
          CleanSnapshot(Directory + '\' + Entry.Name);
      until not FindNext(Entry);
    finally
      FindClose(Entry);
    end;
  end;
  RemoveDir(Directory);
  CleanSnapshot(InstallRoot + '\.inno\defaults');
end;
