import struct,sys,csv
def read(path):
    d=open(path,'rb').read()
    assert d[:5]==b'MLVLG',d[:8]
    ver=struct.unpack('>h',d[6:8])[0]
    if ver==1:
        ts,infoStart,dataStart,recLen,nF=struct.unpack('>ihihh',d[8:22]); off=22; fsz=55
    else:
        ts,infoStart,dataStart,recLen,nF=struct.unpack('>iIIHH',d[8:24]); off=24; fsz=89
    fields=[]
    for i in range(nF):
        b=d[off+i*fsz:off+(i+1)*fsz]
        typ=b[0]; name=b[1:35].split(b'\0')[0].decode('latin1'); units=b[35:45].split(b'\0')[0].decode('latin1')
        scale,trans=struct.unpack('>ff',b[46:54])
        fields.append((typ,name,units,scale,trans))
    fmt={0:'B',1:'b',2:'H',3:'h',4:'I',5:'i',6:'q',7:'f',10:'B',11:'H',12:'I',16:'B'}
    sf='>'+''.join(fmt[f[0]] for f in fields)
    rl=struct.calcsize(sf)
    assert rl==recLen,(rl,recLen)
    rows=[];p=dataStart
    while p<len(d):
        bt=d[p]
        if bt==0:
            if p+4+rl>len(d): break
            v=struct.unpack(sf,d[p+4:p+4+rl])
            rows.append([(x+f[4])*f[3] for x,f in zip(v,fields)])
            p+=4+rl+1
        elif bt==1:
            p+=4+50+0
        else: break
    return ver,fields,rows
if __name__=='__main__':
    ver,f,r=read(sys.argv[1])
    w=csv.writer(open(sys.argv[2],'w',newline=''))
    w.writerow([x[1] for x in f]); w.writerow([x[2] for x in f]); w.writerows(r)
    print(sys.argv[1].split('/')[-1],'v',ver,len(f),'fields',len(r),'rows')
