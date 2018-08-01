package com.mapswithme.maps.bookmarks;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.support.annotation.CallSuper;
import android.support.annotation.NonNull;
import android.support.annotation.StyleRes;
import android.support.v4.app.Fragment;

import com.mapswithme.maps.R;
import com.mapswithme.maps.base.BaseToolbarActivity;
import com.mapswithme.maps.bookmarks.data.BookmarkManager;
import com.mapswithme.util.ThemeUtils;

public class BookmarkCategoriesActivity extends BaseToolbarActivity
{
  public static final int REQ_CODE_DOWNLOAD_BOOKMARK_CATEGORY = 102;

  public static void startForResult(@NonNull Activity context, int initialPage)
  {
    Intent intent = new Intent(context, BookmarkCategoriesActivity.class);
    Bundle args = new Bundle();
    args.putInt(BookmarkCategoriesPagerFragment.ARG_CATEGORIES_PAGE, initialPage);
    intent.putExtras(args);
    context.startActivityForResult(intent, REQ_CODE_DOWNLOAD_BOOKMARK_CATEGORY);
  }

  @CallSuper
  @Override
  public void onResume()
  {
    super.onResume();

    // Disable all notifications in BM on appearance of this activity.
    // It allows to significantly improve performance in case of bookmarks
    // modification. All notifications will be sent on activity's disappearance.
    BookmarkManager.INSTANCE.setNotificationsEnabled(false);
  }

  @CallSuper
  @Override
  public void onPause()
  {
    // Allow to send all notifications in BM.
    BookmarkManager.INSTANCE.setNotificationsEnabled(true);

    super.onPause();
  }

  @Override
  @StyleRes
  public int getThemeResourceId(@NonNull String theme)
  {
    return ThemeUtils.getWindowBgThemeResourceId(theme);
  }

  @Override
  protected Class<? extends Fragment> getFragmentClass()
  {
    return BookmarkCategoriesPagerFragment.class;
  }

  @Override
  protected int getContentLayoutResId()
  {
    return R.layout.bookmarks_activity;
  }
}
