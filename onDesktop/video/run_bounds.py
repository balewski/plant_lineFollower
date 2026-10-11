"""Use first reported controller stop, excluding final STOP retry cleanup."""
def run_bounds(rows):
    armed=int(next(r['t_ns'] for r in rows if r['event']=='armed'))
    final=int(next(r['t_ns'] for r in rows if r['event']=='final_stop'))
    stops=[r for r in rows if r['event']=='status' and int(r['c']) in (3,4) and int(r['t_ns'])>=armed]
    if stops:
        first=min(stops,key=lambda r:int(r['t_ns']))
        return armed,min(final,int(first['t_ns'])),('STOP: freshness' if int(first['d'])==2 else 'STOP: controller')
    return armed,final,'STOP CONFIRMED'
