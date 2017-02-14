

TEST(Gfx, TestDeviceResetHandling)
{
  // create device
  DeviceManagerDx::Init();
  InitializeDevices();
  // create handle
  Maybe<TextureFactoryIdentifier> newIdentifier;
  newIdentifier = ResetCompositorImpl(aBackendHints);
  *aOutNewIdentifier = newIdentifier;
  // create syncobject
  RefPtr<SyncObject> mSyncObject;
  mSyncObject = SyncObject::CreateSyncObject(newIdentifier.mSyncHandle);
  // call init
  mSyncObject->FinalizeFrame();
}
