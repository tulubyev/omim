#include "routing/async_router.hpp"

#include "geometry/mercator.hpp"

#include "base/logging.hpp"
#include "base/macros.hpp"
#include "base/string_utils.hpp"
#include "base/timer.hpp"

#include "std/functional.hpp"
#include "std/utility.hpp"

using namespace std;
using namespace std::placeholders;

namespace routing
{

namespace
{
map<string, string> PrepareStatisticsData(string const & routerName,
                                          m2::PointD const & startPoint, m2::PointD const & startDirection,
                                          m2::PointD const & finalPoint)
{
  // Coordinates precision in 5 digits after comma corresponds to metres (0,00001degree ~ 1meter),
  // therefore we round coordinates up to 5 digits after comma.
  int constexpr precision = 5;

  return {{"name", routerName},
          {"startLon", strings::to_string_dac(MercatorBounds::XToLon(startPoint.x), precision)},
          {"startLat", strings::to_string_dac(MercatorBounds::YToLat(startPoint.y), precision)},
          {"startDirectionX", strings::to_string_dac(startDirection.x, precision)},
          {"startDirectionY", strings::to_string_dac(startDirection.y, precision)},
          {"finalLon", strings::to_string_dac(MercatorBounds::XToLon(finalPoint.x), precision)},
          {"finalLat", strings::to_string_dac(MercatorBounds::YToLat(finalPoint.y), precision)}};
}

void SendStatistics(m2::PointD const & startPoint, m2::PointD const & startDirection,
                    m2::PointD const & finalPoint, RouterResultCode resultCode, double routeLenM,
                    double elapsedSec, RoutingStatisticsCallback const & routingStatisticsCallback,
                    string const & routerName)
{
  if (nullptr == routingStatisticsCallback)
    return;

  map<string, string> statistics = PrepareStatisticsData(routerName, startPoint, startDirection, finalPoint);
  statistics.emplace("result", DebugPrint(resultCode));
  statistics.emplace("elapsed", strings::to_string(elapsedSec));

  if (RouterResultCode::NoError == resultCode)
    statistics.emplace("distance", strings::to_string(routeLenM));

  routingStatisticsCallback(statistics);
}

void SendStatistics(m2::PointD const & startPoint, m2::PointD const & startDirection,
                    m2::PointD const & finalPoint, string const & exceptionMessage,
                    RoutingStatisticsCallback const & routingStatisticsCallback,
                    string const & routerName)
{
  if (nullptr == routingStatisticsCallback)
    return;

  map<string, string> statistics = PrepareStatisticsData(routerName, startPoint, startDirection, finalPoint);
  statistics.emplace("exception", exceptionMessage);

  routingStatisticsCallback(statistics);
}
}  // namespace

// ----------------------------------------------------------------------------------------------------------------------------

AsyncRouter::RouterDelegateProxy::RouterDelegateProxy(ReadyCallbackOwnership const & onReady,
                                                      NeedMoreMapsCallback const & m_onNeedMoreMaps,
                                                      RemoveRouteCallback const & removeRoute,
                                                      PointCheckCallback const & onPointCheck,
                                                      ProgressCallback const & onProgress,
                                                      uint32_t timeoutSec)
  : m_onReadyOwnership(onReady)
  , m_onNeedMoreMaps(m_onNeedMoreMaps)
  , m_removeRoute(removeRoute)
  , m_onPointCheck(onPointCheck)
  , m_onProgress(onProgress)
{
  m_delegate.Reset();
  m_delegate.SetPointCheckCallback(bind(&RouterDelegateProxy::OnPointCheck, this, _1));
  m_delegate.SetProgressCallback(bind(&RouterDelegateProxy::OnProgress, this, _1));
  m_delegate.SetTimeout(timeoutSec);
}

void AsyncRouter::RouterDelegateProxy::OnReady(shared_ptr<Route> route, RouterResultCode resultCode)
{
  if (!m_onReadyOwnership)
    return;
  {
    lock_guard<mutex> l(m_guard);
    if (m_delegate.IsCancelled())
      return;
  }
  m_onReadyOwnership(move(route), resultCode);
}

void AsyncRouter::RouterDelegateProxy::OnNeedMoreMaps(uint64_t routeId,
                                                      vector<string> const & absentCounties)
{
  if (!m_onNeedMoreMaps)
    return;
  {
    lock_guard<mutex> l(m_guard);
    if (m_delegate.IsCancelled())
      return;
  }
  m_onNeedMoreMaps(routeId, absentCounties);
}

void AsyncRouter::RouterDelegateProxy::OnRemoveRoute(RouterResultCode resultCode)
{
  if (!m_removeRoute)
    return;
  {
    lock_guard<mutex> l(m_guard);
    if (m_delegate.IsCancelled())
      return;
  }
  m_removeRoute(resultCode);
}

void AsyncRouter::RouterDelegateProxy::Cancel()
{
  lock_guard<mutex> l(m_guard);
  m_delegate.Cancel();
}

void AsyncRouter::RouterDelegateProxy::OnProgress(float progress)
{
  ProgressCallback onProgress = nullptr;

  {
    lock_guard<mutex> l(m_guard);
    if (!m_onProgress)
      return;

    if (m_delegate.IsCancelled())
      return;

    onProgress = m_onProgress;
    GetPlatform().RunTask(Platform::Thread::Gui, [onProgress, progress]() {
      onProgress(progress);
    });
  }
}

void AsyncRouter::RouterDelegateProxy::OnPointCheck(m2::PointD const & pt)
{
#ifdef SHOW_ROUTE_DEBUG_MARKS
  PointCheckCallback onPointCheck = nullptr;
  m2::PointD point;
  {
    lock_guard<mutex> l(m_guard);
    CHECK(m_onPointCheck, ());

    if (m_delegate.IsCancelled())
      return;

    onPointCheck = m_onPointCheck;
    point = pt;
  }

  GetPlatform().RunTask(Platform::Thread::Gui, [onPointCheck, point]() { onPointCheck(point); });
#endif
}

// -------------------------------------------------------------------------------------------------

AsyncRouter::AsyncRouter(RoutingStatisticsCallback const & routingStatisticsCallback,
                         PointCheckCallback const & pointCheckCallback)
  : m_threadExit(false)
  , m_hasRequest(false)
  , m_clearState(false)
  , m_routingStatisticsCallback(routingStatisticsCallback)
  , m_pointCheckCallback(pointCheckCallback)
{
  m_thread = threads::SimpleThread(&AsyncRouter::ThreadFunc, this);
}

AsyncRouter::~AsyncRouter()
{
  {
    unique_lock<mutex> ul(m_guard);

    ResetDelegate();

    m_threadExit = true;
    m_threadCondVar.notify_one();
  }

  m_thread.join();
}

void AsyncRouter::SetRouter(unique_ptr<IRouter> && router, unique_ptr<IOnlineFetcher> && fetcher)
{
  unique_lock<mutex> ul(m_guard);

  ResetDelegate();

  m_router = move(router);
  m_absentFetcher = move(fetcher);
}

void AsyncRouter::CalculateRoute(Checkpoints const & checkpoints, m2::PointD const & direction,
                                 bool adjustToPrevRoute, ReadyCallbackOwnership const & readyCallback,
                                 NeedMoreMapsCallback const & needMoreMapsCallback,
                                 RemoveRouteCallback const & removeRouteCallback,
                                 ProgressCallback const & progressCallback,
                                 uint32_t timeoutSec)
{
  unique_lock<mutex> ul(m_guard);

  m_checkpoints = checkpoints;
  m_startDirection = direction;
  m_adjustToPrevRoute = adjustToPrevRoute;

  ResetDelegate();

  m_delegate = make_shared<RouterDelegateProxy>(readyCallback, needMoreMapsCallback, removeRouteCallback,
                                                m_pointCheckCallback, progressCallback, timeoutSec);

  m_hasRequest = true;
  m_threadCondVar.notify_one();
}

void AsyncRouter::ClearState()
{
  unique_lock<mutex> ul(m_guard);

  m_clearState = true;
  m_threadCondVar.notify_one();

  ResetDelegate();
}

void AsyncRouter::LogCode(RouterResultCode code, double const elapsedSec)
{
  switch (code)
  {
    case RouterResultCode::StartPointNotFound:
      LOG(LWARNING, ("Can't find start or end node"));
      break;
    case RouterResultCode::EndPointNotFound:
      LOG(LWARNING, ("Can't find end point node"));
      break;
    case RouterResultCode::PointsInDifferentMWM:
      LOG(LWARNING, ("Points are in different MWMs"));
      break;
    case RouterResultCode::RouteNotFound:
      LOG(LWARNING, ("Route not found"));
      break;
    case RouterResultCode::RouteFileNotExist:
      LOG(LWARNING, ("There is no routing file"));
      break;
    case RouterResultCode::NeedMoreMaps:
      LOG(LINFO,
          ("Routing can find a better way with additional maps, elapsed seconds:", elapsedSec));
      break;
    case RouterResultCode::Cancelled:
      LOG(LINFO, ("Route calculation cancelled, elapsed seconds:", elapsedSec));
      break;
    case RouterResultCode::NoError:
      LOG(LINFO, ("Route found, elapsed seconds:", elapsedSec));
      break;
    case RouterResultCode::NoCurrentPosition:
      LOG(LINFO, ("No current position"));
      break;
    case RouterResultCode::InconsistentMWMandRoute:
      LOG(LINFO, ("Inconsistent mwm and route"));
      break;
    case RouterResultCode::InternalError:
      LOG(LINFO, ("Internal error"));
      break;
    case RouterResultCode::FileTooOld:
      LOG(LINFO, ("File too old"));
      break;
    case RouterResultCode::IntermediatePointNotFound:
      LOG(LWARNING, ("Can't find intermediate point node"));
      break;
    case RouterResultCode::TransitRouteNotFoundNoNetwork:
      LOG(LWARNING, ("No transit route is found because there's no transit network in the mwm of "
                     "the route point"));
      break;
    case RouterResultCode::TransitRouteNotFoundTooLongPedestrian:
      LOG(LWARNING, ("No transit route is found because pedestrian way is too long"));
      break;
    case RouterResultCode::RouteNotFoundRedressRouteError:
      LOG(LWARNING, ("Route not found because of a redress route error"));
      break;
  }
}

void AsyncRouter::RunOnReadyOnGuiThread(shared_ptr<RouterDelegateProxy> delegate,
                                        shared_ptr<Route> route, RouterResultCode code)
{
  RunOnGuiThread([delegate, route, code]() {delegate->OnReady(route, code);});
}

void AsyncRouter::ResetDelegate()
{
  if (m_delegate)
  {
    m_delegate->Cancel();
    m_delegate.reset();
  }
}

void AsyncRouter::ThreadFunc()
{
  while (true)
  {
    {
      unique_lock<mutex> ul(m_guard);
      m_threadCondVar.wait(ul, [this](){ return m_threadExit || m_hasRequest || m_clearState; });

      if (m_clearState && m_router)
      {
        m_router->ClearState();
        m_clearState = false;
      }

      if (m_threadExit)
        break;

      if (!m_hasRequest)
        continue;
    }

    CalculateRoute();
  }
}

void AsyncRouter::CalculateRoute()
{
  shared_ptr<RouterDelegateProxy> delegate;
  Checkpoints checkpoints;
  m2::PointD startDirection;
  bool adjustToPrevRoute = false;
  shared_ptr<IOnlineFetcher> absentFetcher;
  shared_ptr<IRouter> router;
  uint64_t routeId = m_routeCounter;
  RoutingStatisticsCallback routingStatisticsCallback = nullptr;
  string routerName;

  {
    unique_lock<mutex> ul(m_guard);

    bool hasRequest = m_hasRequest;
    m_hasRequest = false;
    if (!hasRequest)
      return;
    if (!m_router)
      return;
    if (!m_delegate)
      return;

    checkpoints = m_checkpoints;
    startDirection = m_startDirection;
    adjustToPrevRoute = m_adjustToPrevRoute;
    delegate = m_delegate;
    router = m_router;
    absentFetcher = m_absentFetcher;
    routeId = ++m_routeCounter;

    routingStatisticsCallback = m_routingStatisticsCallback;
    routerName = router->GetName();
  }

  shared_ptr<Route> route = make_shared<Route>(router->GetName(), routeId);
  RouterResultCode code;

  my::Timer timer;
  double elapsedSec = 0.0;

  try
  {
    LOG(LINFO, ("Calculating the route. checkpoints:", checkpoints, "startDirection:",
                startDirection, "router name:", router->GetName()));

    if (absentFetcher)
      absentFetcher->GenerateRequest(checkpoints);

    // Run basic request.
    code = router->CalculateRoute(checkpoints, startDirection, adjustToPrevRoute,
                                  delegate->GetDelegate(), *route);

    elapsedSec = timer.ElapsedSeconds(); // routing time
    LogCode(code, elapsedSec);
  }
  catch (RootException const & e)
  {
    code = RouterResultCode::InternalError;
    LOG(LERROR, ("Exception happened while calculating route:", e.Msg()));
    RunOnGuiThread([checkpoints, startDirection, e, routingStatisticsCallback, routerName]() {
      SendStatistics(checkpoints.GetStart(), startDirection, checkpoints.GetFinish(), e.Msg(),
                     routingStatisticsCallback, routerName);
    });
    // Note. After call of this method |route| should be used only on ui thread.
    // And |route| should stop using on routing background thread, in this method.
    RunOnReadyOnGuiThread(delegate, route, code);
    return;
  }

  double const routeLengthM = route->GetTotalDistanceMeters();
  RunOnGuiThread([checkpoints, startDirection, code, routeLengthM, elapsedSec,
                    routingStatisticsCallback, routerName]() {
    SendStatistics(checkpoints.GetStart(), startDirection, checkpoints.GetFinish(), code,
                   routeLengthM, elapsedSec, routingStatisticsCallback, routerName);
  });

  // Draw route without waiting network latency.
  if (code == RouterResultCode::NoError)
  {
    // Note. After call of this method |route| should be used only on ui thread.
    // And |route| should stop using on routing background thread, in this method.
    RunOnReadyOnGuiThread(delegate, route, code);
  }

  bool const needFetchAbsent = (code != RouterResultCode::Cancelled);

  // Check online response if we have.
  vector<string> absent;
  if (absentFetcher && needFetchAbsent)
    absentFetcher->GetAbsentCountries(absent);

  if (!absent.empty() && code == RouterResultCode::NoError)
    code = RouterResultCode::NeedMoreMaps;

  elapsedSec = timer.ElapsedSeconds(); // routing time + absents fetch time
  LogCode(code, elapsedSec);

  // Call callback only if we have some new data.
  if (code != RouterResultCode::NoError)
  {
    if (code == RouterResultCode::NeedMoreMaps)
       RunOnGuiThread([delegate, routeId, absent]() { delegate->OnNeedMoreMaps(routeId, absent); });
    else
      RunOnGuiThread([delegate, code]() { delegate->OnRemoveRoute(code); });
  }
}
}  // namespace routing
