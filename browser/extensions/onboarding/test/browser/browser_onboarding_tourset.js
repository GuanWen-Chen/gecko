/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

add_task(async function test_onboarding_default_new_tourset() {
  resetOnboardingDefaultState();
  let tabs = [];
  for (let url of URLs) {
    let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser);
    await BrowserTestUtils.loadURI(tab.linkedBrowser, url);
    await promiseOnboardingOverlayLoaded(tab.linkedBrowser);
    await BrowserTestUtils.synthesizeMouseAtCenter("#onboarding-overlay-button", {}, tab.linkedBrowser);
    await promiseOnboardingOverlayOpened(tab.linkedBrowser);
    tabs.push(tab);
  }

  let doc = content && content.document;
  let doms = doc.querySelectorAll(".onboarding-tour-item");
  is(doms.length, TOUR_IDs.length, "has exact tour numbers");
  doms.forEach((dom, idx) => {
    is(TOUR_IDs[idx], dom.id, "contain defined onboarding id");
  });

  for (let i = tabs.length - 1; i >= 0; --i) {
    let tab = tabs[i];
    await BrowserTestUtils.removeTab(tab);
  }
});

add_task(async function test_onboarding_custom_new_tourset() {
  const CUSTOM_NEW_TOURs = [
    "onboarding-tour-private-browsing",
    "onboarding-tour-addons",
    "onboarding-tour-customize",
  ];

  resetOnboardingDefaultState();
  await SpecialPowers.pushPrefEnv({set: [
    ["browser.onboarding.tour-type", "new"],
    ["browser.onboarding.tourset-version", 1],
    ["browser.onboarding.seen-tourset-version", 1],
    ["browser.onboarding.newtour", "private,addons,customize"],
  ]});

  let tabs = [];
  for (let url of URLs) {
    let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser);
    await BrowserTestUtils.loadURI(tab.linkedBrowser, url);
    await promiseOnboardingOverlayLoaded(tab.linkedBrowser);
    await BrowserTestUtils.synthesizeMouseAtCenter("#onboarding-overlay-button", {}, tab.linkedBrowser);
    await promiseOnboardingOverlayOpened(tab.linkedBrowser);
    tabs.push(tab);
  }

  let doc = content && content.document;
  let doms = doc.querySelectorAll(".onboarding-tour-item");
  is(doms.length, CUSTOM_NEW_TOURs.length, "has exact tour numbers");
  doms.forEach((dom, idx) => {
    is(CUSTOM_NEW_TOURs[idx], dom.id, "contain defined onboarding id");
  });

  for (let i = tabs.length - 1; i >= 0; --i) {
    let tab = tabs[i];
    await BrowserTestUtils.removeTab(tab);
  }
});

add_task(async function test_onboarding_custom_update_tourset() {
  const CUSTOM_UPDATE_TOURs = [
    "onboarding-tour-customize",
    "onboarding-tour-private-browsing",
    "onboarding-tour-addons",
  ];
  resetOnboardingDefaultState();
  await SpecialPowers.pushPrefEnv({set: [
    ["browser.onboarding.tour-type", "update"],
    ["browser.onboarding.tourset-version", 1],
    ["browser.onboarding.seen-tourset-version", 1],
    ["browser.onboarding.updatetour", "customize,private,addons"],
  ]});

  let tabs = [];
  for (let url of URLs) {
    let tab = await BrowserTestUtils.openNewForegroundTab(gBrowser);
    await BrowserTestUtils.loadURI(tab.linkedBrowser, url);
    await promiseOnboardingOverlayLoaded(tab.linkedBrowser);
    await BrowserTestUtils.synthesizeMouseAtCenter("#onboarding-overlay-button", {}, tab.linkedBrowser);
    await promiseOnboardingOverlayOpened(tab.linkedBrowser);
    tabs.push(tab);
  }

  let doc = content && content.document;
  let doms = doc.querySelectorAll(".onboarding-tour-item");
  is(doms.length, CUSTOM_UPDATE_TOURs.length, "has exact tour numbers");
  doms.forEach((dom, idx) => {
    is(CUSTOM_UPDATE_TOURs[idx], dom.id, "contain defined onboarding id");
  });

  for (let i = tabs.length - 1; i >= 0; --i) {
    let tab = tabs[i];
    await BrowserTestUtils.removeTab(tab);
  }
});
